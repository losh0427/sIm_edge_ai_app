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

class Inference {
public:
    ~Inference();
    bool load_model(const std::string& model_path, int num_threads = 4);
    // input: uint8 RGB pixels, size must be input_w * input_h * 3
    std::vector<Detection> run(const uint8_t* input_data, int input_size);
    int input_width() const { return input_w_; }
    int input_height() const { return input_h_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    int input_w_ = 0, input_h_ = 0;
};
