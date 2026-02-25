#pragma once
#include "inference.h"
#include <cstdint>

// Pre-allocated buffer must fit the largest supported model input.
// SSD MobileNet v2: 300x300x3 = 270,000 B
// YOLOv8n:         640x640x3 = 1,228,800 B
static constexpr int kMaxModelInputSize = 640 * 640 * 3;

// Original frame: 640 * 480 * 3 = 921,600 bytes
static constexpr int kOrigWidth  = 640;
static constexpr int kOrigHeight = 480;
static constexpr int kOrigChannels = 3;
static constexpr int kOrigDataSize = kOrigWidth * kOrigHeight * kOrigChannels;

static constexpr int kMaxDetections = 100;

// Slot passed from recv thread → infer thread (Ring A)
struct InferSlot {
    uint8_t input_data[kMaxModelInputSize];  // resized model input (up to 640x640x3)
    uint8_t orig_data[kOrigDataSize];     // original frame for thumbnail later
    int     frame_number;

    // Benchmark timestamps (nanoseconds, CLOCK_MONOTONIC)
    int64_t ts_capture_start;
    int64_t ts_capture_end;
    int64_t ts_preprocess_end;
};

// Slot passed from infer thread → upload thread (Ring B)
struct UploadSlot {
    Detection detections[kMaxDetections];
    int       detection_count;
    uint8_t   orig_data[kOrigDataSize];   // original frame for thumbnail encoding
    int       frame_number;
    float     inference_latency_ms;

    // Benchmark timestamps carried from upstream
    int64_t ts_capture_start;
    int64_t ts_capture_us;      // capture duration
    int64_t ts_preprocess_us;   // preprocess duration
    int64_t ts_inference_us;    // inference duration
    int64_t ts_nms_us;          // NMS duration
};
