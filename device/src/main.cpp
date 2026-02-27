#if defined(HAL_USE_OPENCV)
#include "hal/opencv_frame_source.h"
#elif defined(HAL_USE_FILE)
#include "hal/file_frame_source.h"
#endif

#include "src/inference.h"
#include "src/grpc_client.h"
#include "src/labels.h"
#include "src/pipeline.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <csignal>
#include <memory>

static Pipeline* g_pipeline = nullptr;

void sig_handler(int) {
    if (g_pipeline) g_pipeline->request_shutdown();
}

int main(int argc, char** argv) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    // Config from env
    std::string model_path  = getenv("MODEL_PATH")  ? getenv("MODEL_PATH")  : "/app/models/ssd_mobilenet_v2.tflite";
    std::string labels_path = getenv("LABELS_PATH") ? getenv("LABELS_PATH") : "/app/models/coco_labels.txt";
    std::string server_addr = getenv("SERVER_ADDR") ? getenv("SERVER_ADDR") : "server:50051";
    std::string edge_id     = getenv("EDGE_ID")     ? getenv("EDGE_ID")     : "edge-1";
    float conf_threshold    = getenv("CONF_THRESH") ? atof(getenv("CONF_THRESH")) : 0.4f;
    float iou_threshold     = getenv("IOU_THRESH")  ? atof(getenv("IOU_THRESH"))  : 0.45f;

    auto labels = load_labels(labels_path);
    fprintf(stderr, "Loaded %zu labels\n", labels.size());

#ifdef PIPELINE_LEVEL
    fprintf(stderr, "Pipeline level: %d\n", PIPELINE_LEVEL);
#endif

    // Init HAL
    std::unique_ptr<IFrameSource> source;

#if defined(HAL_USE_OPENCV)
    int cam_index = getenv("CAM_INDEX") ? atoi(getenv("CAM_INDEX")) : 0;
    source = std::make_unique<OpenCVFrameSource>(cam_index, 640, 480);
#elif defined(HAL_USE_FILE)
    std::string input_dir = getenv("INPUT_DIR") ? getenv("INPUT_DIR") : "/app/data/test_frames";
    source = std::make_unique<FileFrameSource>(input_dir, 640, 480);
#endif

    if (!source->open()) {
        fprintf(stderr, "Failed to open frame source\n");
        return 1;
    }
    fprintf(stderr, "Frame source: %s\n", source->describe().c_str());

    // Init inference
    Inference infer;
    if (!infer.load_model(model_path)) return 1;

    // Init gRPC
    GrpcClient grpc;
    grpc.connect(server_addr);

    // Benchmark config
    std::string bench_csv = getenv("BENCH_CSV") ? getenv("BENCH_CSV") : "";
    int bench_warmup      = getenv("BENCH_WARMUP") ? atoi(getenv("BENCH_WARMUP")) : 100;

    // Build pipeline config and run
    PipelineConfig config;
    config.source         = source.get();
    config.inference      = &infer;
    config.grpc           = &grpc;
    config.edge_id        = edge_id;
    config.hal_desc       = source->describe();
    config.conf_threshold = conf_threshold;
    config.iou_threshold  = iou_threshold;
    config.labels         = labels;
    config.bench_csv      = bench_csv;
    config.bench_warmup   = bench_warmup;

    Pipeline pipeline;
    g_pipeline = &pipeline;

    if (pipeline.start(config) != 0) {
        fprintf(stderr, "Failed to start pipeline\n");
        return 1;
    }

    pipeline.stop();
    source->close();
    g_pipeline = nullptr;
    fprintf(stderr, "\nDone.\n");
    return 0;
}
