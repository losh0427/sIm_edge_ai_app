#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

struct Detection {
    float x_min, y_min, x_max, y_max;  // normalized [0,1]
    int class_id;
    float confidence;
};

enum class ModelType {
    SSD_MOBILENET,  // 4 outputs: boxes, classes, scores, count (post-NMS from model)
    YOLOV8,         // 1 output: [1, 4+num_classes, num_anchors] (raw, NMS applied manually)
};

class Inference {
public:
    ~Inference();
    bool load_model(const std::string& model_path, int num_threads = 4);
    // input: uint8 RGB pixels, size must be input_w * input_h * 3
    // conf_threshold: pre-filter before NMS (reduces NMS workload for YOLO's 8400 anchors)
    std::vector<Detection> run(const uint8_t* input_data, int input_size,
                               float conf_threshold = 0.1f);
    int input_width()  const { return input_w_; }
    int input_height() const { return input_h_; }
    ModelType model_type() const { return model_type_; }

private:
    std::vector<Detection> parse_ssd_output(float conf_threshold);
    std::vector<Detection> parse_yolov8_output(float conf_threshold);

    struct Impl;
    Impl* impl_     = nullptr;
    int input_w_    = 0, input_h_ = 0;
    bool input_is_float_ = false;  // true for float32 input models (YOLOv8 float)
    ModelType model_type_ = ModelType::SSD_MOBILENET;
};
