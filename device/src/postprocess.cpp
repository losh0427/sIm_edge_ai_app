#include "postprocess.h"
#include <algorithm>
#include <cmath>

namespace postprocess {

float compute_iou(const Detection& a, const Detection& b) {
    float ix0 = std::max(a.x_min, b.x_min);
    float iy0 = std::max(a.y_min, b.y_min);
    float ix1 = std::min(a.x_max, b.x_max);
    float iy1 = std::min(a.y_max, b.y_max);

    float inter_w = std::max(0.0f, ix1 - ix0);
    float inter_h = std::max(0.0f, iy1 - iy0);
    float inter_area = inter_w * inter_h;

    float area_a = (a.x_max - a.x_min) * (a.y_max - a.y_min);
    float area_b = (b.x_max - b.x_min) * (b.y_max - b.y_min);
    float union_area = area_a + area_b - inter_area;

    if (union_area <= 0.0f) return 0.0f;
    return inter_area / union_area;
}

std::vector<Detection> nms_filter(std::vector<Detection>& detections,
                                   float iou_threshold, float score_threshold) {
    // Filter by score
    std::vector<Detection> filtered;
    for (auto& d : detections)
        if (d.confidence >= score_threshold) filtered.push_back(d);

    // Sort by confidence descending
    std::sort(filtered.begin(), filtered.end(),
              [](const Detection& a, const Detection& b) {
                  return a.confidence > b.confidence;
              });

    std::vector<bool> suppressed(filtered.size(), false);
    std::vector<Detection> results;

    for (size_t i = 0; i < filtered.size(); i++) {
        if (suppressed[i]) continue;
        results.push_back(filtered[i]);
        for (size_t j = i + 1; j < filtered.size(); j++) {
            if (suppressed[j]) continue;
            if (compute_iou(filtered[i], filtered[j]) > iou_threshold)
                suppressed[j] = true;
        }
    }
    return results;
}

}
