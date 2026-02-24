#!/bin/bash
set -e
mkdir -p models
wget -q -O models/ssd_mobilenet_v2.tflite \
    "https://raw.githubusercontent.com/google-coral/test_data/master/ssd_mobilenet_v2_coco_quant_postprocess.tflite"
echo "Model downloaded: $(ls -lh models/ssd_mobilenet_v2.tflite | awk '{print $5}')"
