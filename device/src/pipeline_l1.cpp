// Level 1: 3x std::thread + ThreadSafeQueue (unbounded)
// Uses std::queue + std::mutex + std::condition_variable, no capacity limit.
// Each frame allocates std::vector for resized data and original data.
// Problem: if recv is faster than infer, queue grows without bound (OOM risk).

#include "src/pipeline.h"
#include "src/preprocess.h"
#include "src/postprocess.h"
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <algorithm>

static inline int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ── Unbounded thread-safe queue ──
template <typename T>
class ThreadSafeQueue {
public:
    void push(T item) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            queue_.push(std::move(item));
        }
        cv_.notify_one();
    }

    // Returns false only when shutdown and queue is empty
    bool pop(T& out) {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [&]{ return !queue_.empty() || shutdown_; });
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            shutdown_ = true;
        }
        cv_.notify_all();
    }

    size_t size() {
        std::lock_guard<std::mutex> lk(mtx_);
        return queue_.size();
    }

private:
    std::queue<T> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool shutdown_ = false;
};

// ── Inter-thread data items (heap-allocated per frame) ──
struct InferItem {
    std::vector<uint8_t> resized_data;  // model input (heap alloc)
    std::vector<uint8_t> orig_data;     // original frame for thumbnail
    int orig_w, orig_h;
    int frame_number;
    int64_t ts_capture_start;
    int64_t ts_capture_us;
    int64_t ts_preprocess_us;
};

struct UploadItem {
    std::vector<Detection> detections;
    std::vector<uint8_t> orig_data;
    int orig_w, orig_h;
    int frame_number;
    float inference_latency_ms;
    int64_t ts_capture_start;
    int64_t ts_capture_us;
    int64_t ts_preprocess_us;
    int64_t ts_inference_us;
    int64_t ts_nms_us;
};

struct Pipeline::Impl {
    PipelineConfig config;

    ThreadSafeQueue<InferItem>  queue_a;  // recv → infer
    ThreadSafeQueue<UploadItem> queue_b;  // infer → upload

    std::thread recv_thread;
    std::thread infer_thread;
    std::thread upload_thread;
    bool threads_started = false;

    void recv_loop() {
        fprintf(stderr, "[recv] thread started (std::thread)\n");
        auto& cfg = config;
        int frame_num = 0;

        while (true) {
            int64_t t_cap_start = now_ns();

            Frame frame;
            if (!cfg.source->next_frame(frame)) {
                fprintf(stderr, "[recv] no more frames\n");
                break;
            }
            int64_t t_cap_end = now_ns();

            frame_num++;

            // Resize — returns std::vector (heap alloc every frame)
            auto resized = preprocess::resize(frame,
                                              cfg.inference->input_width(),
                                              cfg.inference->input_height());
            int64_t t_preproc_end = now_ns();

            InferItem item;
            item.resized_data    = std::move(resized);
            item.orig_data       = frame.data;  // copy (heap alloc)
            item.orig_w          = frame.width;
            item.orig_h          = frame.height;
            item.frame_number    = frame_num;
            item.ts_capture_start = t_cap_start;
            item.ts_capture_us   = (t_cap_end - t_cap_start) / 1000;
            item.ts_preprocess_us = (t_preproc_end - t_cap_end) / 1000;

            queue_a.push(std::move(item));
        }

        queue_a.shutdown();
        fprintf(stderr, "[recv] thread exiting (frames: %d)\n", frame_num);
    }

    void infer_loop() {
        fprintf(stderr, "[infer] thread started (std::thread)\n");
        auto& cfg = config;
        int processed = 0;

        InferItem item;
        while (queue_a.pop(item)) {
            int64_t t_infer_start = now_ns();
            auto raw_dets = cfg.inference->run(item.resized_data.data(),
                                               (int)item.resized_data.size(),
                                               cfg.conf_threshold);
            int64_t t_infer_end = now_ns();
            float latency_ms = (t_infer_end - t_infer_start) / 1e6f;

            int64_t t_nms_start = now_ns();
            auto filtered = postprocess::nms_filter(raw_dets, cfg.iou_threshold, cfg.conf_threshold);
            int64_t t_nms_end = now_ns();

            UploadItem up;
            up.detections          = std::move(filtered);
            up.orig_data           = std::move(item.orig_data);
            up.orig_w              = item.orig_w;
            up.orig_h              = item.orig_h;
            up.frame_number        = item.frame_number;
            up.inference_latency_ms = latency_ms;
            up.ts_capture_start    = item.ts_capture_start;
            up.ts_capture_us       = item.ts_capture_us;
            up.ts_preprocess_us    = item.ts_preprocess_us;
            up.ts_inference_us     = (t_infer_end - t_infer_start) / 1000;
            up.ts_nms_us           = (t_nms_end - t_nms_start) / 1000;

            queue_b.push(std::move(up));
            processed++;
        }

        queue_b.shutdown();
        fprintf(stderr, "[infer] thread exiting (frames: %d)\n", processed);
    }

    void upload_loop() {
        fprintf(stderr, "[upload] thread started (std::thread)\n");
        auto& cfg = config;
        auto start_time = std::chrono::steady_clock::now();
        int uploaded = 0;

        FILE* bench_fp = nullptr;
        if (!cfg.bench_csv.empty()) {
            bench_fp = fopen(cfg.bench_csv.c_str(), "w");
            if (bench_fp) {
                fprintf(bench_fp, "frame,capture_us,preprocess_us,inference_us,"
                                  "nms_us,thumbnail_us,grpc_us,e2e_us,num_dets\n");
                fprintf(stderr, "[upload] Benchmark CSV: %s (warmup=%d)\n",
                        cfg.bench_csv.c_str(), cfg.bench_warmup);
            }
        }

        UploadItem item;
        while (queue_b.pop(item)) {
            int64_t t_thumb_start = now_ns();
            auto thumbnail = preprocess::encode_thumbnail_raw(
                item.orig_data.data(), item.orig_w, item.orig_h, 3);
            int64_t t_thumb_end = now_ns();

            int64_t t_grpc_start = now_ns();
            cfg.grpc->send_detection(cfg.edge_id, item.detections, thumbnail,
                                     item.inference_latency_ms, item.frame_number,
                                     cfg.hal_desc, item.orig_w, item.orig_h);
            int64_t t_grpc_end = now_ns();

            uploaded++;
            auto elapsed = std::chrono::duration<float>(
                std::chrono::steady_clock::now() - start_time).count();
            float fps = (elapsed > 0.0f) ? uploaded / elapsed : 0.0f;

            fprintf(stderr, "\r[upload] Frame %d | %.1f FPS | %.1f ms | %d dets | qA=%zu qB=%zu   ",
                    item.frame_number, fps, item.inference_latency_ms,
                    (int)item.detections.size(), queue_a.size(), queue_b.size());

            for (auto& d : item.detections) {
                const char* lbl = (d.class_id >= 0 && d.class_id < (int)cfg.labels.size())
                                  ? cfg.labels[d.class_id].c_str() : "???";
                fprintf(stderr, "\n  [%s] %.2f (%.3f,%.3f)-(%.3f,%.3f)",
                        lbl, d.confidence, d.x_min, d.y_min, d.x_max, d.y_max);
            }

            if (bench_fp && item.frame_number > cfg.bench_warmup) {
                int64_t thumb_us = (t_thumb_end - t_thumb_start) / 1000;
                int64_t grpc_us  = (t_grpc_end - t_grpc_start) / 1000;
                int64_t e2e_us   = (t_grpc_end - item.ts_capture_start) / 1000;

                fprintf(bench_fp, "%d,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%d\n",
                        item.frame_number,
                        (long long)item.ts_capture_us,
                        (long long)item.ts_preprocess_us,
                        (long long)item.ts_inference_us,
                        (long long)item.ts_nms_us,
                        (long long)thumb_us,
                        (long long)grpc_us,
                        (long long)e2e_us,
                        (int)item.detections.size());
            }
        }

        if (bench_fp) {
            fclose(bench_fp);
            fprintf(stderr, "\n[upload] Benchmark CSV saved\n");
        }

        fprintf(stderr, "\n[upload] thread exiting (frames: %d)\n", uploaded);
    }
};

// ── Pipeline public interface ──

int Pipeline::start(const PipelineConfig& config) {
    impl_ = new Impl();
    impl_->config = config;

    // Start consumers first, then producer
    impl_->upload_thread = std::thread([this]{ impl_->upload_loop(); });
    impl_->infer_thread  = std::thread([this]{ impl_->infer_loop(); });
    impl_->recv_thread   = std::thread([this]{ impl_->recv_loop(); });
    impl_->threads_started = true;

    fprintf(stderr, "Pipeline L1: 3 std::threads started (recv → infer → upload)\n");
    return 0;
}

void Pipeline::stop() {
    if (!impl_) return;
    if (impl_->threads_started) {
        impl_->recv_thread.join();
        impl_->infer_thread.join();
        impl_->upload_thread.join();
        fprintf(stderr, "Pipeline L1: all threads joined\n");
    }
    delete impl_;
    impl_ = nullptr;
}

void Pipeline::request_shutdown() {
    if (!impl_) return;
    impl_->queue_a.shutdown();
    impl_->queue_b.shutdown();
}
