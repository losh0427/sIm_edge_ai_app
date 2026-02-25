#pragma once
#include "hal/i_frame_source.h"
#include <vector>
#include <cstdint>

namespace preprocess {

// Resize RGB frame to model input size, output as uint8 (for INT8 model)
std::vector<uint8_t> resize(const Frame& frame, int target_w, int target_h);

// Encode frame as JPEG thumbnail for server visualization (aspect-ratio preserving)
std::vector<uint8_t> encode_thumbnail(const Frame& frame, int max_w = 480, int quality = 80);

// Resize into pre-allocated buffer (for pipeline zero-malloc path)
void resize_into(const Frame& frame, int target_w, int target_h, uint8_t* out_buf, int out_size);

// Encode thumbnail from raw RGB pointer (for pipeline upload thread, aspect-ratio preserving)
std::vector<uint8_t> encode_thumbnail_raw(const uint8_t* rgb_data, int w, int h, int channels,
                                          int max_w = 480, int quality = 80);

}
