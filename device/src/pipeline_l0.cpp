// Level 0: Single-threaded pipeline — simplest possible implementation
// No threads, no queues, no pre-allocation. Just a blocking loop.
// Each frame: capture → resize (heap alloc) → infer → NMS → thumbnail → gRPC

#include "src/pipeline.h"
#include "src/preprocess.h"
#include "src/postprocess.h"
#include <chrono>
#include <cstdio>
#include <cstring>

static inline int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Pipeline::Impl {
    PipelineConfig config;
    volatile bool shutdown_requested = false;
};

int Pipeline::start(const PipelineConfig& config) {
    impl_ = new Impl();
    impl_->config = config;
    auto& cfg = impl_->config;

    fprintf(stderr, "Pipeline L0: single-threaded loop starting\n");

    auto start_time = std::chrono::steady_clock::now();
    int frame_num = 0;

    // Benchmark CSV
    FILE* bench_fp = nullptr;
    if (!cfg.bench_csv.empty()) {
        bench_fp = fopen(cfg.bench_csv.c_str(), "w");
        if (bench_fp) {
            fprintf(bench_fp, "frame,capture_us,preprocess_us,inference_us,"
                              "nms_us,thumbnail_us,grpc_us,e2e_us,num_dets\n");
            fprintf(stderr, "[L0] Benchmark CSV: %s (warmup=%d)\n",
                    cfg.bench_csv.c_str(), cfg.bench_warmup);
        }
    }

    while (!impl_->shutdown_requested) {
        // 1. Capture
        int64_t t_cap_start = now_ns();
        Frame frame;
        if (!cfg.source->next_frame(frame)) {
            fprintf(stderr, "[L0] no more frames\n");
            break;
        }
        int64_t t_cap_end = now_ns();

        frame_num++;

        // 2. Preprocess — resize returns std::vector (heap alloc every frame)
        auto resized = preprocess::resize(frame,
                                          cfg.inference->input_width(),
                                          cfg.inference->input_height());
        int64_t t_preproc_end = now_ns();

        // 3. Inference
        int64_t t_infer_start = now_ns();
        auto raw_dets = cfg.inference->run(resized.data(), (int)resized.size(),
                                           cfg.conf_threshold);
        int64_t t_infer_end = now_ns();
        float latency_ms = (t_infer_end - t_infer_start) / 1e6f;

        // 4. NMS
        int64_t t_nms_start = now_ns();
        auto filtered = postprocess::nms_filter(raw_dets, cfg.iou_threshold, cfg.conf_threshold);
        int64_t t_nms_end = now_ns();

        // 5. Thumbnail
        int64_t t_thumb_start = now_ns();
        auto thumbnail = preprocess::encode_thumbnail(frame);
        int64_t t_thumb_end = now_ns();

        // 6. gRPC send
        int64_t t_grpc_start = now_ns();
        cfg.grpc->send_detection(cfg.edge_id, filtered, thumbnail,
                                 latency_ms, frame_num,
                                 cfg.hal_desc, frame.width, frame.height);
        int64_t t_grpc_end = now_ns();

        // Log
        auto elapsed = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - start_time).count();
        float fps = (elapsed > 0.0f) ? frame_num / elapsed : 0.0f;

        fprintf(stderr, "\r[L0] Frame %d | %.1f FPS | %.1f ms | %d detections   ",
                frame_num, fps, latency_ms, (int)filtered.size());

        for (auto& d : filtered) {
            const char* lbl = (d.class_id >= 0 && d.class_id < (int)cfg.labels.size())
                              ? cfg.labels[d.class_id].c_str() : "???";
            fprintf(stderr, "\n  [%s] %.2f (%.3f,%.3f)-(%.3f,%.3f)",
                    lbl, d.confidence, d.x_min, d.y_min, d.x_max, d.y_max);
        }

        // Benchmark CSV
        if (bench_fp && frame_num > cfg.bench_warmup) {
            int64_t cap_us     = (t_cap_end - t_cap_start) / 1000;
            int64_t preproc_us = (t_preproc_end - t_cap_end) / 1000;
            int64_t infer_us   = (t_infer_end - t_infer_start) / 1000;
            int64_t nms_us     = (t_nms_end - t_nms_start) / 1000;
            int64_t thumb_us   = (t_thumb_end - t_thumb_start) / 1000;
            int64_t grpc_us    = (t_grpc_end - t_grpc_start) / 1000;
            int64_t e2e_us     = (t_grpc_end - t_cap_start) / 1000;

            fprintf(bench_fp, "%d,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%d\n",
                    frame_num,
                    (long long)cap_us, (long long)preproc_us,
                    (long long)infer_us, (long long)nms_us,
                    (long long)thumb_us, (long long)grpc_us,
                    (long long)e2e_us, (int)filtered.size());
        }
    }

    if (bench_fp) {
        fclose(bench_fp);
        fprintf(stderr, "\n[L0] Benchmark CSV saved\n");
    }

    fprintf(stderr, "\n[L0] done (frames: %d)\n", frame_num);
    return 0;
}

void Pipeline::stop() {
    delete impl_;
    impl_ = nullptr;
}

void Pipeline::request_shutdown() {
    if (impl_) impl_->shutdown_requested = true;
}
