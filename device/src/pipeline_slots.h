#pragma once
#include "inference.h"
#include <cstdint>

// Model input: 300 * 300 * 3 = 270,000 bytes
static constexpr int kModelInputSize = 300 * 300 * 3;

// Original frame: 640 * 480 * 3 = 921,600 bytes
static constexpr int kOrigWidth  = 640;
static constexpr int kOrigHeight = 480;
static constexpr int kOrigChannels = 3;
static constexpr int kOrigDataSize = kOrigWidth * kOrigHeight * kOrigChannels;

static constexpr int kMaxDetections = 100;

// Slot passed from recv thread → infer thread (Ring A)
struct InferSlot {
    uint8_t input_data[kModelInputSize];  // resized model input (300x300x3)
    uint8_t orig_data[kOrigDataSize];     // original frame for thumbnail later
    int     frame_number;
};

// Slot passed from infer thread → upload thread (Ring B)
struct UploadSlot {
    Detection detections[kMaxDetections];
    int       detection_count;
    uint8_t   orig_data[kOrigDataSize];   // original frame for thumbnail encoding
    int       frame_number;
    float     inference_latency_ms;
};
