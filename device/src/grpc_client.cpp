#include "grpc_client.h"
#include "edge_ai.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <cstdio>
#include <chrono>

struct GrpcClient::Impl {
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<edgeai::EdgeService::Stub> stub;
};

GrpcClient::~GrpcClient() { delete impl_; }

bool GrpcClient::connect(const std::string& server_addr) {
    impl_ = new Impl();
    impl_->channel = grpc::CreateChannel(server_addr, grpc::InsecureChannelCredentials());
    impl_->stub = edgeai::EdgeService::NewStub(impl_->channel);
    fprintf(stderr, "gRPC connected to %s\n", server_addr.c_str());
    return true;
}

bool GrpcClient::send_detection(const std::string& edge_id,
                                 const std::vector<Detection>& detections,
                                 const std::vector<uint8_t>& thumbnail_jpeg,
                                 float latency_ms, int frame_num,
                                 const std::string& hal_desc,
                                 int orig_w, int orig_h) {
    edgeai::DetectionFrame req;
    req.set_edge_id(edge_id);
    req.set_timestamp_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    req.set_inference_latency_ms(latency_ms);
    req.set_frame_number(frame_num);
    req.set_hal_backend(hal_desc);
    req.set_original_width(orig_w);
    req.set_original_height(orig_h);
    req.set_thumbnail_jpeg(thumbnail_jpeg.data(), thumbnail_jpeg.size());

    for (auto& d : detections) {
        auto* box = req.add_boxes();
        box->set_x_min(d.x_min);
        box->set_y_min(d.y_min);
        box->set_x_max(d.x_max);
        box->set_y_max(d.y_max);
        box->set_class_id(d.class_id);
        box->set_confidence(d.confidence);
    }

    edgeai::ServerResponse resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

    auto status = impl_->stub->ReportDetection(&ctx, req, &resp);
    if (!status.ok()) {
        fprintf(stderr, "gRPC send failed: %s\n", status.error_message().c_str());
        return false;
    }
    return resp.ok();
}
