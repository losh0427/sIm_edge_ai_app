#pragma once
#include "hal/i_frame_source.h"
#include "src/inference.h"
#include "src/grpc_client.h"
#include <string>
#include <vector>

struct PipelineConfig {
    IFrameSource* source;
    Inference*    inference;
    GrpcClient*   grpc;
    std::string   edge_id;
    std::string   hal_desc;
    float         conf_threshold;
    float         iou_threshold;
    std::vector<std::string> labels;
    std::string   bench_csv;    // if non-empty, write per-frame CSV
    int           bench_warmup; // frames to skip before recording
};

class Pipeline {
public:
    int  start(const PipelineConfig& config);
    void stop();
    void request_shutdown();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
