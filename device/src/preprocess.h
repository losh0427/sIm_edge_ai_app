#pragma once
#include "hal/i_frame_source.h"
#include <vector>
#include <cstdint>

namespace preprocess {

// Resize RGB frame to model input size, output as uint8 (for INT8 model)
std::vector<uint8_t> resize(const Frame& frame, int target_w, int target_h);

// Encode frame as JPEG thumbnail for server visualization
std::vector<uint8_t> encode_thumbnail(const Frame& frame, int thumb_w = 160, int thumb_h = 120, int quality = 70);

}
