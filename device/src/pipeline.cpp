#include "src/pipeline.h"
#include "src/ring_buffer.h"
#include "src/pipeline_slots.h"
#include "src/preprocess.h"
#include "src/postprocess.h"
#include <pthread.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <algorithm>

static constexpr int kRingSlots = 4;

struct Pipeline::Impl {
    PipelineConfig config;

    RingBuffer<InferSlot,  kRingSlots> ring_a;  // recv → infer
    RingBuffer<UploadSlot, kRingSlots> ring_b;  // infer → upload

    pthread_t recv_tid;
    pthread_t infer_tid;
    pthread_t upload_tid;
    bool threads_started = false;
};

// ── static entry points (C-style for pthread_create) ──

void* Pipeline::recv_entry(void* arg) {
    static_cast<Pipeline*>(arg)->recv_loop();
    return nullptr;
}

void* Pipeline::infer_entry(void* arg) {
    static_cast<Pipeline*>(arg)->infer_loop();
    return nullptr;
}

void* Pipeline::upload_entry(void* arg) {
    static_cast<Pipeline*>(arg)->upload_loop();
    return nullptr;
}

// ── start / stop ──

int Pipeline::start(const PipelineConfig& config) {
    impl_ = new Impl();
    impl_->config = config;

    if (impl_->ring_a.init() != 0) {
        fprintf(stderr, "Pipeline: ring_a init failed\n");
        delete impl_; impl_ = nullptr;
        return -1;
    }
    if (impl_->ring_b.init() != 0) {
        fprintf(stderr, "Pipeline: ring_b init failed\n");
        impl_->ring_a.destroy();
        delete impl_; impl_ = nullptr;
        return -1;
    }

    // Start consumers first, then producer (so consumers are ready before data flows)
    if (pthread_create(&impl_->upload_tid, nullptr, upload_entry, this) != 0) {
        fprintf(stderr, "Pipeline: pthread_create(upload) failed\n");
        impl_->ring_a.destroy();
        impl_->ring_b.destroy();
        delete impl_; impl_ = nullptr;
        return -1;
    }
    if (pthread_create(&impl_->infer_tid, nullptr, infer_entry, this) != 0) {
        fprintf(stderr, "Pipeline: pthread_create(infer) failed\n");
        impl_->ring_b.shutdown();
        pthread_join(impl_->upload_tid, nullptr);
        impl_->ring_a.destroy();
        impl_->ring_b.destroy();
        delete impl_; impl_ = nullptr;
        return -1;
    }
    if (pthread_create(&impl_->recv_tid, nullptr, recv_entry, this) != 0) {
        fprintf(stderr, "Pipeline: pthread_create(recv) failed\n");
        impl_->ring_a.shutdown();
        impl_->ring_b.shutdown();
        pthread_join(impl_->infer_tid, nullptr);
        pthread_join(impl_->upload_tid, nullptr);
        impl_->ring_a.destroy();
        impl_->ring_b.destroy();
        delete impl_; impl_ = nullptr;
        return -1;
    }
    impl_->threads_started = true;

    fprintf(stderr, "Pipeline: 3 threads started (recv → infer → upload)\n");
    return 0;
}

void Pipeline::stop() {
    if (!impl_) return;
    if (impl_->threads_started) {
        pthread_join(impl_->recv_tid, nullptr);
        pthread_join(impl_->infer_tid, nullptr);
        pthread_join(impl_->upload_tid, nullptr);
        fprintf(stderr, "Pipeline: all threads joined\n");
    }
    impl_->ring_a.destroy();
    impl_->ring_b.destroy();
    delete impl_;
    impl_ = nullptr;
}

void Pipeline::request_shutdown() {
    if (!impl_) return;
    impl_->ring_a.shutdown();
    impl_->ring_b.shutdown();
}

// ── Thread 1: recv loop ──

void Pipeline::recv_loop() {
    fprintf(stderr, "[recv] thread started\n");
    auto& cfg = impl_->config;
    int frame_num = 0;

    while (!impl_->ring_a.is_shutdown()) {
        Frame frame;
        if (!cfg.source->next_frame(frame)) {
            fprintf(stderr, "[recv] no more frames, shutting down pipeline\n");
            impl_->ring_a.shutdown();
            break;
        }

        InferSlot* slot = impl_->ring_a.acquire_write_slot();
        if (!slot) break;

        frame_num++;
        slot->frame_number = frame_num;

        // Resize into pre-allocated slot buffer
        int model_input_size = cfg.inference->input_width() * cfg.inference->input_height() * 3;
        preprocess::resize_into(frame,
                                cfg.inference->input_width(),
                                cfg.inference->input_height(),
                                slot->input_data, model_input_size);

        // Copy original frame data for thumbnail encoding later
        int copy_size = std::min((int)frame.data.size(), kOrigDataSize);
        memcpy(slot->orig_data, frame.data.data(), copy_size);

        impl_->ring_a.commit_write_slot();
    }

    // Signal downstream: no more data coming
    impl_->ring_a.shutdown();
    fprintf(stderr, "[recv] thread exiting (frames read: %d)\n", frame_num);
}

// ── Thread 2: infer loop ──

void Pipeline::infer_loop() {
    fprintf(stderr, "[infer] thread started\n");
    auto& cfg = impl_->config;
    int processed = 0;

    while (true) {
        InferSlot* in_slot = impl_->ring_a.acquire_read_slot();
        if (!in_slot) break;

        // Run inference (pre-filter with conf_threshold to reduce NMS workload)
        int actual_input_size = cfg.inference->input_width() * cfg.inference->input_height() * 3;
        auto t0 = std::chrono::steady_clock::now();
        auto raw_dets = cfg.inference->run(in_slot->input_data, actual_input_size,
                                           cfg.conf_threshold);
        auto t1 = std::chrono::steady_clock::now();
        float latency_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();

        // NMS + confidence filter
        auto filtered = postprocess::nms_filter(raw_dets, cfg.iou_threshold, cfg.conf_threshold);

        // Acquire upload slot
        UploadSlot* out_slot = impl_->ring_b.acquire_write_slot();
        if (!out_slot) {
            impl_->ring_a.commit_read_slot();
            break;
        }

        // Fill upload slot
        out_slot->frame_number = in_slot->frame_number;
        out_slot->inference_latency_ms = latency_ms;
        out_slot->detection_count = std::min((int)filtered.size(), kMaxDetections);
        for (int i = 0; i < out_slot->detection_count; i++) {
            out_slot->detections[i] = filtered[i];
        }
        memcpy(out_slot->orig_data, in_slot->orig_data, kOrigDataSize);

        impl_->ring_a.commit_read_slot();
        impl_->ring_b.commit_write_slot();

        processed++;
    }

    // Signal downstream
    impl_->ring_b.shutdown();
    fprintf(stderr, "[infer] thread exiting (frames processed: %d)\n", processed);
}

// ── Thread 3: upload loop ──

void Pipeline::upload_loop() {
    fprintf(stderr, "[upload] thread started\n");
    auto& cfg = impl_->config;
    auto start_time = std::chrono::steady_clock::now();
    int uploaded = 0;

    while (true) {
        UploadSlot* slot = impl_->ring_b.acquire_read_slot();
        if (!slot) break;

        // Build detection vector for gRPC
        std::vector<Detection> dets(slot->detections, slot->detections + slot->detection_count);

        // Encode thumbnail from original frame
        auto thumbnail = preprocess::encode_thumbnail_raw(
            slot->orig_data, kOrigWidth, kOrigHeight, kOrigChannels);

        // Log
        uploaded++;
        auto elapsed = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - start_time).count();
        float fps = (elapsed > 0.0f) ? uploaded / elapsed : 0.0f;

        fprintf(stderr, "\r[upload] Frame %d | %.1f FPS | %.1f ms | %d detections   ",
                slot->frame_number, fps, slot->inference_latency_ms, slot->detection_count);

        for (int i = 0; i < slot->detection_count; i++) {
            auto& d = slot->detections[i];
            const char* lbl = (d.class_id >= 0 && d.class_id < (int)cfg.labels.size())
                              ? cfg.labels[d.class_id].c_str() : "???";
            fprintf(stderr, "\n  [%s] %.2f (%.3f,%.3f)-(%.3f,%.3f)",
                    lbl, d.confidence, d.x_min, d.y_min, d.x_max, d.y_max);
        }

        // Send via gRPC
        cfg.grpc->send_detection(cfg.edge_id, dets, thumbnail,
                                 slot->inference_latency_ms, slot->frame_number,
                                 cfg.hal_desc, kOrigWidth, kOrigHeight);

        impl_->ring_b.commit_read_slot();
    }

    fprintf(stderr, "\n[upload] thread exiting (frames uploaded: %d)\n", uploaded);
}
