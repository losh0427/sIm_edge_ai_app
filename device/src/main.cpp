#include "hal/file_frame_source.h"
#include "src/preprocess.h"
#include "src/inference.h"
#include "src/postprocess.h"
#include "src/grpc_client.h"
#include "src/labels.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string>
#include <thread>
#include <csignal>

static volatile bool running = true;
void sig_handler(int) { running = false; }

int main(int argc, char** argv) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    // Config from env
    std::string input_dir   = getenv("INPUT_DIR")   ? getenv("INPUT_DIR")   : "/app/data/test_frames";
    std::string model_path  = getenv("MODEL_PATH")  ? getenv("MODEL_PATH")  : "/app/models/ssd_mobilenet_v2.tflite";
    std::string labels_path = getenv("LABELS_PATH") ? getenv("LABELS_PATH") : "/app/models/coco_labels.txt";
    std::string server_addr = getenv("SERVER_ADDR") ? getenv("SERVER_ADDR") : "server:50051";
    std::string edge_id     = getenv("EDGE_ID")     ? getenv("EDGE_ID")     : "edge-1";
    float conf_threshold    = getenv("CONF_THRESH") ? atof(getenv("CONF_THRESH")) : 0.4f;

    auto labels = load_labels(labels_path);
    fprintf(stderr, "Loaded %zu labels\n", labels.size());

    // Init HAL
    FileFrameSource source(input_dir, 640, 480);
    if (!source.open()) {
        fprintf(stderr, "Failed to open frame source: %s\n", input_dir.c_str());
        return 1;
    }
    fprintf(stderr, "Frame source: %s\n", source.describe().c_str());

    // Init inference
    Inference infer;
    if (!infer.load_model(model_path)) return 1;

    // Init gRPC
    GrpcClient grpc;
    grpc.connect(server_addr);

    int frame_num = 0;
    auto start_time = std::chrono::steady_clock::now();

    while (running) {
        Frame frame;
        if (!source.next_frame(frame)) {
            fprintf(stderr, "No more frames\n");
            break;
        }

        // Preprocess: resize to model input
        auto input = preprocess::resize(frame, infer.input_width(), infer.input_height());

        // Inference
        auto t0 = std::chrono::steady_clock::now();
        auto detections = infer.run(input.data(), input.size());
        auto t1 = std::chrono::steady_clock::now();
        float latency_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();

        // Filter by confidence
        std::vector<Detection> filtered;
        for (auto& d : detections)
            if (d.confidence >= conf_threshold) filtered.push_back(d);

        // Thumbnail
        auto thumbnail = preprocess::encode_thumbnail(frame);

        // Assign labels
        for (auto& d : filtered) {
            // SSD class_id is 0-indexed, but some models are 1-indexed
            int idx = d.class_id;
            if (idx >= 0 && idx < (int)labels.size())
                ; // label available
        }

        // Print
        frame_num++;
        auto elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - start_time).count();
        float fps = frame_num / elapsed;
        fprintf(stderr, "\rFrame %d | %.1f FPS | %.1f ms | %zu detections   ",
                frame_num, fps, latency_ms, filtered.size());

        for (auto& d : filtered) {
            std::string lbl = (d.class_id >= 0 && d.class_id < (int)labels.size())
                              ? labels[d.class_id] : "???";
            fprintf(stderr, "\n  [%s] %.2f (%.3f,%.3f)-(%.3f,%.3f)",
                    lbl.c_str(), d.confidence, d.x_min, d.y_min, d.x_max, d.y_max);
        }

        // Send via gRPC
        grpc.send_detection(edge_id, filtered, thumbnail, latency_ms,
                           frame_num, source.describe(), frame.width, frame.height);

        // Throttle to ~15 FPS for file source
        std::this_thread::sleep_for(std::chrono::milliseconds(66));
    }

    source.close();
    fprintf(stderr, "\nDone. %d frames processed.\n", frame_num);
    return 0;
}
