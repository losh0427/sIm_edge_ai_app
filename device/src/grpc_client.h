#pragma once
#include "inference.h"
#include <string>
#include <vector>
#include <memory>

class GrpcClient {
public:
    ~GrpcClient();
    bool connect(const std::string& server_addr);
    bool send_detection(const std::string& edge_id,
                        const std::vector<Detection>& detections,
                        const std::vector<uint8_t>& thumbnail_jpeg,
                        float latency_ms, int frame_num,
                        const std::string& hal_desc,
                        int orig_w, int orig_h);
private:
    struct Impl;
    Impl* impl_ = nullptr;
};
