#!/usr/bin/env bash
# Install build dependencies, configure the v4l2loopback virtual camera, build, and
# install camfx into ~/.local with a start menu entry and an XDG autostart entry.
#
# Options:
#   --no-autostart   do not start the app on login
#   --skip-apt       do not install system packages
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
PREFIX="$HOME/.local"
AUTOSTART=1
APT=1
for arg in "$@"; do
    case "$arg" in
        --no-autostart) AUTOSTART=0 ;;
        --skip-apt) APT=0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

VIDEO_NR=10
LABEL="CamFX"

if [[ $APT == 1 ]]; then
    echo "== System packages"
    sudo apt-get install -y build-essential cmake ninja-build pkg-config \
        qt6-base-dev libopencv-dev \
        libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
        gstreamer1.0-tools gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
        v4l2loopback-dkms v4l-utils
fi

echo "== Virtual camera (/dev/video$VIDEO_NR)"
# exclusive_caps=1 is required for Chrome/WebRTC to list the device
echo "v4l2loopback" | sudo tee /etc/modules-load.d/v4l2loopback.conf >/dev/null
echo "options v4l2loopback devices=1 video_nr=$VIDEO_NR card_label=\"$LABEL\" exclusive_caps=1" \
    | sudo tee /etc/modprobe.d/v4l2loopback.conf >/dev/null
if [[ ! -e /dev/video$VIDEO_NR ]]; then
    if lsmod | grep -q '^v4l2loopback'; then
        sudo modprobe -r v4l2loopback || echo "v4l2loopback is in use with other options; reboot to apply."
    fi
    sudo modprobe v4l2loopback || true
fi

echo "== Dependencies"
"$ROOT/scripts/fetch_deps.sh"

echo "== Build"
cmake -S "$ROOT" -B "$ROOT/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --build "$ROOT/build"
cmake --install "$ROOT/build"

# Refresh the menu and the icon cache so the start menu entry appears at once
update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true
gtk-update-icon-cache -q -t "$PREFIX/share/icons/hicolor" 2>/dev/null || true

if [[ $AUTOSTART == 1 ]]; then
    mkdir -p "$HOME/.config/autostart"
    # Start in the tray only, without opening the window
    sed 's| --show||' "$PREFIX/share/applications/camfx.desktop" > "$HOME/.config/autostart/camfx.desktop"
    echo "Autostart enabled."
fi

echo "Installed: $PREFIX/bin/camfx"
ls -l /dev/video$VIDEO_NR 2>/dev/null || echo "Note: /dev/video$VIDEO_NR does not exist yet; reboot once."
