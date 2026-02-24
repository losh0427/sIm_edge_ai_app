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
};

class Pipeline {
public:
    int  start(const PipelineConfig& config);
    void stop();
    void request_shutdown();

private:
    static void* recv_entry(void* arg);
    static void* infer_entry(void* arg);
    static void* upload_entry(void* arg);

    void recv_loop();
    void infer_loop();
    void upload_loop();

    struct Impl;
    Impl* impl_ = nullptr;
};
