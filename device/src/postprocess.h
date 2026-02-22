#pragma once
#include "inference.h"
#include <vector>

namespace postprocess {

float compute_iou(const Detection& a, const Detection& b);

// NMS: keep highest confidence, suppress overlapping boxes
std::vector<Detection> nms_filter(std::vector<Detection>& detections,
                                   float iou_threshold = 0.5f,
                                   float score_threshold = 0.3f);

}
