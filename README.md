# CamFX

A tray application for Linux that applies effects to the webcam image and publishes the
result as a virtual camera ("CamFX", `/dev/video10`). Video call applications
(Zoom, Teams, Chrome, Firefox) select this camera like any other camera.

Effects:

- **Tone**: brightness, contrast, saturation, warmth, gamma, sharpen
- **Skin**: edge-preserving skin smoothing and brightening, limited to skin areas
- **Background**: blur, image, or solid color
- **Look**: built-in color looks and `.cube` 3D LUT files

## Install

```sh
./install.sh            # asks for sudo: packages, v4l2loopback setup
./install.sh --no-autostart
```

The script:

1. Installs the build dependencies and `v4l2loopback-dkms` (skip with `--skip-apt`).
2. Writes `/etc/modules-load.d/v4l2loopback.conf` and `/etc/modprobe.d/v4l2loopback.conf`,
   so `/dev/video10` exists after every boot. `exclusive_caps=1` is required for Chrome.
3. Downloads ONNX Runtime (GPU build) into `third_party/` (`scripts/fetch_deps.sh`).
4. Builds and installs into `~/.local`: the binary, a start menu entry ("CamFX") with an
   icon, and an XDG autostart entry that starts the app in the tray.

Requirements: Ubuntu 24.04 or similar, Qt 6, OpenCV 4, GStreamer 1.20+. An NVIDIA GPU
with CUDA 13 and cuDNN 9 is optional; without it, segmentation runs on the CPU. For CUDA 12,
use `ORT_FLAVOR=cuda12 scripts/fetch_deps.sh`.

## Use

- Start "CamFX" from the start menu, or left-click the tray icon, to open the settings
  window. It shows a live preview of the output and the current latency. The preview
  grows with the window; drag the divider to give it more or less space.
- Only one instance runs. A second start opens the window of the running instance.
- Right-click for presets, "Bypass effects", "Settings…" and "Quit".
- The real camera is open only while an application uses the virtual camera or the
  settings window is open. Otherwise the virtual camera shows a "Camera paused" frame,
  and the camera LED is off.

Tray icon colors: grey = idle, pink = live, orange = live with effects bypassed.

Configuration is in `~/.config/camfx/`:

- `presets/*.json`: one file per preset
- `luts/*.cube`: additional color looks
- `state.json`: current parameters, preset, camera

## Latency

Measure the latency headless (the camera must not be in use):

```sh
camfx --bench --seconds 10                 # all effects
camfx --bench --preset Natural --snapshot out.png
```

The benchmark reads the virtual camera back like a video call application and matches
frames by a sequence number drawn into the image. Stages:

| stage | meaning |
|---|---|
| capture -> app | camera sensor timestamp to decoded frame in the app (includes USB transfer and JPEG decode) |
| mask wait | wait for the segmentation of the current frame |
| effects | color conversion and all effects |
| output | YUY2 conversion and hand-off to `/dev/video10` |
| capture -> consumer app | end to end, as a video call application sees it |

Example on an i9-13950HX with an RTX 3500 Ada, 1280x720 at 30 fps:

| preset | capture -> consumer app (mean) |
|---|---|
| no effects | 42 ms |
| Natural | 50 ms |
| all effects | 55 ms |

About 31 ms of this is inside the camera and the USB transfer: the frame is already that
old when the driver delivers it. A video call application that reads the camera directly
sees the same delay.

Set `CAMFX_PROFILE=1` to print per-element and per-effect timings to stderr.

## Design

```
v4l2src (MJPG 720p30) -> jpegdec -> appsink (I420)
        |
        |  I420 -> BGR + mirror
        |  1/4 size copy --------------------> segmentation thread (ONNX Runtime, CUDA)
        |  sharpen, skin filter coefficients        | person + skin masks
        |  <----------------------------------------+
        |  skin smoothing (mask bounding box only)
        |  background blur / image / color
        |  composite + tone curves + color 3D LUT (one pass)
        |  BGR -> YUY2
        v
appsrc -> v4l2sink (/dev/video10)
```

- `appsink max-buffers=1 drop=true`: the app always processes the newest frame. If
  processing is too slow, frames are dropped instead of queued.
- Segmentation uses MediaPipe's `selfie_multiclass_256x256` model, converted to ONNX
  with softmax and class reduction appended (`scripts/add_postprocess.py`). One inference
  gives both the background mask and the skin mask.
- The mask-independent work runs while the GPU segments the same frame.
- A `v4l2loopback` client-usage event tells the app when a consumer starts or stops
  streaming.

## Build manually

```sh
scripts/fetch_deps.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/camfx --show
```

Options: `--device` (virtual camera), `--camera` (input), `--model`, `--show` (open the
settings window at start).
