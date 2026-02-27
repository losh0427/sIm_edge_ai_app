// Level 2: 3x pthread_create + PosixQueue (bounded, sem_t + pthread_mutex_t)
// Introduces backpressure via bounded capacity (8 slots).
// Still uses std::vector per frame (heap alloc), but producer blocks when queue is full.
// Demonstrates POSIX API usage and why backpressure matters.

#include "src/pipeline.h"
#include "src/preprocess.h"
#include "src/postprocess.h"
#include <pthread.h>
#include <semaphore.h>
#include <queue>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <algorithm>

static inline int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ── Bounded queue with POSIX primitives ──
template <typename T>
class PosixQueue {
public:
    int init(int capacity) {
        capacity_ = capacity;
        if (sem_init(&empty_slots_, 0, capacity) != 0) return -1;
        if (sem_init(&fill_count_, 0, 0) != 0) {
            sem_destroy(&empty_slots_);
            return -1;
        }
        if (pthread_mutex_init(&mtx_, nullptr) != 0) {
            sem_destroy(&empty_slots_);
            sem_destroy(&fill_count_);
            return -1;
        }
        shutdown_ = false;
        return 0;
    }

    void destroy() {
        sem_destroy(&empty_slots_);
        sem_destroy(&fill_count_);
        pthread_mutex_destroy(&mtx_);
    }

    // Blocks until a slot is available or shutdown
    bool push(T item) {
        while (sem_wait(&empty_slots_) != 0) {
            if (shutdown_) return false;
        }
        if (shutdown_) return false;

        pthread_mutex_lock(&mtx_);
        queue_.push(std::move(item));
        pthread_mutex_unlock(&mtx_);

        sem_post(&fill_count_);
        return true;
    }

    // Blocks until an item is available or shutdown
    bool pop(T& out) {
        while (sem_wait(&fill_count_) != 0) {
            if (shutdown_) return false;
        }
        if (shutdown_) {
            // Drain remaining items
            pthread_mutex_lock(&mtx_);
            if (!queue_.empty()) {
                out = std::move(queue_.front());
                queue_.pop();
                pthread_mutex_unlock(&mtx_);
                return true;
            }
            pthread_mutex_unlock(&mtx_);
            return false;
        }

        pthread_mutex_lock(&mtx_);
        out = std::move(queue_.front());
        queue_.pop();
        pthread_mutex_unlock(&mtx_);

        sem_post(&empty_slots_);
        return true;
    }

    void shutdown() {
        shutdown_ = true;
        sem_post(&empty_slots_);
        sem_post(&fill_count_);
    }

    size_t size() {
        pthread_mutex_lock(&mtx_);
        size_t s = queue_.size();
        pthread_mutex_unlock(&mtx_);
        return s;
    }

private:
    std::queue<T> queue_;
    sem_t empty_slots_;
    sem_t fill_count_;
    pthread_mutex_t mtx_;
    int capacity_ = 0;
    volatile bool shutdown_ = false;
};

static constexpr int kQueueCapacity = 8;

// ── Inter-thread data items ──
struct InferItem {
    std::vector<uint8_t> resized_data;
    std::vector<uint8_t> orig_data;
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

    PosixQueue<InferItem>  queue_a;
    PosixQueue<UploadItem> queue_b;

    pthread_t recv_tid;
    pthread_t infer_tid;
    pthread_t upload_tid;
    bool threads_started = false;

    static void* recv_entry(void* arg)   { static_cast<Impl*>(arg)->recv_loop(); return nullptr; }
    static void* infer_entry(void* arg)  { static_cast<Impl*>(arg)->infer_loop(); return nullptr; }
    static void* upload_entry(void* arg) { static_cast<Impl*>(arg)->upload_loop(); return nullptr; }

    void recv_loop() {
        fprintf(stderr, "[recv] thread started (pthread, bounded queue=%d)\n", kQueueCapacity);
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

            // Resize — heap alloc per frame
            auto resized = preprocess::resize(frame,
                                              cfg.inference->input_width(),
                                              cfg.inference->input_height());
            int64_t t_preproc_end = now_ns();

            InferItem item;
            item.resized_data     = std::move(resized);
            item.orig_data        = frame.data;
            item.orig_w           = frame.width;
            item.orig_h           = frame.height;
            item.frame_number     = frame_num;
            item.ts_capture_start = t_cap_start;
            item.ts_capture_us    = (t_cap_end - t_cap_start) / 1000;
            item.ts_preprocess_us = (t_preproc_end - t_cap_end) / 1000;

            if (!queue_a.push(std::move(item))) break;  // shutdown
        }

        queue_a.shutdown();
        fprintf(stderr, "[recv] thread exiting (frames: %d)\n", frame_num);
    }

    void infer_loop() {
        fprintf(stderr, "[infer] thread started (pthread)\n");
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

            if (!queue_b.push(std::move(up))) break;
            processed++;
        }

        queue_b.shutdown();
        fprintf(stderr, "[infer] thread exiting (frames: %d)\n", processed);
    }

    void upload_loop() {
        fprintf(stderr, "[upload] thread started (pthread)\n");
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

    if (impl_->queue_a.init(kQueueCapacity) != 0) {
        fprintf(stderr, "Pipeline L2: queue_a init failed\n");
        delete impl_; impl_ = nullptr;
        return -1;
    }
    if (impl_->queue_b.init(kQueueCapacity) != 0) {
        fprintf(stderr, "Pipeline L2: queue_b init failed\n");
        impl_->queue_a.destroy();
        delete impl_; impl_ = nullptr;
        return -1;
    }

    // Start consumers first, then producer
    if (pthread_create(&impl_->upload_tid, nullptr, Impl::upload_entry, impl_) != 0) {
        fprintf(stderr, "Pipeline L2: pthread_create(upload) failed\n");
        impl_->queue_a.destroy();
        impl_->queue_b.destroy();
        delete impl_; impl_ = nullptr;
        return -1;
    }
    if (pthread_create(&impl_->infer_tid, nullptr, Impl::infer_entry, impl_) != 0) {
        fprintf(stderr, "Pipeline L2: pthread_create(infer) failed\n");
        impl_->queue_b.shutdown();
        pthread_join(impl_->upload_tid, nullptr);
        impl_->queue_a.destroy();
        impl_->queue_b.destroy();
        delete impl_; impl_ = nullptr;
        return -1;
    }
    if (pthread_create(&impl_->recv_tid, nullptr, Impl::recv_entry, impl_) != 0) {
        fprintf(stderr, "Pipeline L2: pthread_create(recv) failed\n");
        impl_->queue_a.shutdown();
        impl_->queue_b.shutdown();
        pthread_join(impl_->infer_tid, nullptr);
        pthread_join(impl_->upload_tid, nullptr);
        impl_->queue_a.destroy();
        impl_->queue_b.destroy();
        delete impl_; impl_ = nullptr;
        return -1;
    }
    impl_->threads_started = true;

    fprintf(stderr, "Pipeline L2: 3 pthreads started (bounded queue, capacity=%d)\n", kQueueCapacity);
    return 0;
}

void Pipeline::stop() {
    if (!impl_) return;
    if (impl_->threads_started) {
        pthread_join(impl_->recv_tid, nullptr);
        pthread_join(impl_->infer_tid, nullptr);
        pthread_join(impl_->upload_tid, nullptr);
        fprintf(stderr, "Pipeline L2: all threads joined\n");
    }
    impl_->queue_a.destroy();
    impl_->queue_b.destroy();
    delete impl_;
    impl_ = nullptr;
}

void Pipeline::request_shutdown() {
    if (!impl_) return;
    impl_->queue_a.shutdown();
    impl_->queue_b.shutdown();
}
