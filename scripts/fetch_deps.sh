#!/usr/bin/env bash
# Download ONNX Runtime (GPU build) into third_party/ and make sure the segmentation model exists.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ORT_VERSION="${ORT_VERSION:-1.30.0}"
# cuda13 or cuda12; must match the installed CUDA runtime and cuDNN
ORT_FLAVOR="${ORT_FLAVOR:-cuda13}"
ORT_DIR="$ROOT/third_party/onnxruntime-linux-x64-gpu_${ORT_FLAVOR}-${ORT_VERSION}"
MODEL_URL="https://storage.googleapis.com/mediapipe-models/image_segmenter/selfie_multiclass_256x256/float32/latest/selfie_multiclass_256x256.tflite"

mkdir -p "$ROOT/third_party" "$ROOT/models"

if [[ ! -d "$ORT_DIR" ]]; then
    echo "Downloading ONNX Runtime $ORT_VERSION ($ORT_FLAVOR)"
    curl -fL "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-x64-gpu_${ORT_FLAVOR}-${ORT_VERSION}.tgz" \
        | tar xz -C "$ROOT/third_party"
fi

ONNX="$ROOT/models/selfie_multiclass_256x256.onnx"
if [[ ! -f "$ONNX" ]]; then
    # Convert the MediaPipe TFLite model in a throwaway virtualenv
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT
    curl -fL -o "$TMP/model.tflite" "$MODEL_URL"
    python3 -m venv "$TMP/venv"
    "$TMP/venv/bin/pip" install -q tf2onnx tensorflow-cpu
    "$TMP/venv/bin/python" -m tf2onnx.convert --tflite "$TMP/model.tflite" --output "$TMP/raw.onnx" --opset 17
    # Softmax and class reduction run inside the model (on the GPU)
    "$TMP/venv/bin/python" -I "$ROOT/scripts/add_postprocess.py" "$TMP/raw.onnx" "$ONNX"
fi
echo "Dependencies ready."
