#include "inference.h"
#include "tensorflow/lite/interpreter.h"
#include "tensorflow/lite/kernels/register.h"
#include "tensorflow/lite/model.h"
#include <cstdio>

struct Inference::Impl {
    std::unique_ptr<tflite::FlatBufferModel> model;
    std::unique_ptr<tflite::Interpreter> interpreter;
};

bool Inference::load_model(const std::string& model_path, int num_threads) {
    impl_ = std::make_unique<Impl>();
    impl_->model = tflite::FlatBufferModel::BuildFromFile(model_path.c_str());
    if (!impl_->model) {
        fprintf(stderr, "Failed to load model: %s\n", model_path.c_str());
        return false;
    }

    tflite::ops::builtin::BuiltinOpResolver resolver;
    tflite::InterpreterBuilder builder(*impl_->model, resolver);
    builder(&impl_->interpreter);
    if (!impl_->interpreter) {
        fprintf(stderr, "Failed to build interpreter\n");
        return false;
    }

    impl_->interpreter->SetNumThreads(num_threads);
    if (impl_->interpreter->AllocateTensors() != kTfLiteOk) {
        fprintf(stderr, "Failed to allocate tensors\n");
        return false;
    }

    auto* input = impl_->interpreter->input_tensor(0);
    input_h_ = input->dims->data[1];
    input_w_ = input->dims->data[2];
    fprintf(stderr, "Model loaded: input %dx%d\n", input_w_, input_h_);
    return true;
}

std::vector<Detection> Inference::run(const uint8_t* input_data, int input_size) {
    auto* input_tensor = impl_->interpreter->input_tensor(0);
    memcpy(input_tensor->data.uint8, input_data, input_size);

    if (impl_->interpreter->Invoke() != kTfLiteOk) {
        fprintf(stderr, "Invoke failed\n");
        return {};
    }

    // SSD MobileNet v2 outputs: boxes[1,N,4], classes[1,N], scores[1,N], count[1]
    const float* boxes   = impl_->interpreter->output_tensor(0)->data.f;
    const float* classes = impl_->interpreter->output_tensor(1)->data.f;
    const float* scores  = impl_->interpreter->output_tensor(2)->data.f;
    int count = (int)impl_->interpreter->output_tensor(3)->data.f[0];

    std::vector<Detection> results;
    for (int i = 0; i < count; i++) {
        if (scores[i] < 0.3f) continue;
        Detection d;
        // SSD output: [ymin, xmin, ymax, xmax]
        d.y_min = boxes[i * 4 + 0];
        d.x_min = boxes[i * 4 + 1];
        d.y_max = boxes[i * 4 + 2];
        d.x_max = boxes[i * 4 + 3];
        d.class_id = (int)classes[i];
        d.confidence = scores[i];
        results.push_back(d);
    }
    return results;
}
