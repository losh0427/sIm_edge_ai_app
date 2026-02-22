#!/bin/bash
set -e
mkdir -p models
wget -q -O models/ssd_mobilenet_v2.tflite \
    "https://storage.googleapis.com/download.tensorflow.org/models/tflite/task_library/object_detection/android/lite-model_ssd_mobilenet_v2_1.0_300_integer_quant_1.tflite"
echo "Model downloaded: $(ls -lh models/ssd_mobilenet_v2.tflite | awk '{print $5}')"
