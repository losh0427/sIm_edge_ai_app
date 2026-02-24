#include "inference.h"
#include "tensorflow/lite/interpreter.h"
#include "tensorflow/lite/kernels/register.h"
#include "tensorflow/lite/model.h"
#include <cstdio>
#include <algorithm>

struct Inference::Impl {
    std::unique_ptr<tflite::FlatBufferModel> model;
    std::unique_ptr<tflite::Interpreter> interpreter;
};

Inference::~Inference() { delete impl_; }

bool Inference::load_model(const std::string& model_path, int num_threads) {
    impl_ = new Impl();
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
    input_is_float_ = (input->type == kTfLiteFloat32);

    // Auto-detect model type from number of outputs and tensor shape
    int num_outputs = (int)impl_->interpreter->outputs().size();
    if (num_outputs == 4) {
        model_type_ = ModelType::SSD_MOBILENET;
        fprintf(stderr, "Model: SSD MobileNet  input=%dx%d  type=%s\n",
                input_w_, input_h_, input_is_float_ ? "float32" : "uint8");
    } else if (num_outputs == 1) {
        auto* out = impl_->interpreter->output_tensor(0);
        // YOLOv8 standard export: [1, (4+num_classes), num_anchors]
        if (out->dims->size == 3 && out->dims->data[1] >= 5) {
            model_type_ = ModelType::YOLOV8;
            fprintf(stderr, "Model: YOLOv8  input=%dx%d  type=%s  output=[1,%d,%d]\n",
                    input_w_, input_h_, input_is_float_ ? "float32" : "uint8",
                    out->dims->data[1], out->dims->data[2]);
        } else {
            fprintf(stderr, "Unrecognized single-output model, treating as SSD\n");
            model_type_ = ModelType::SSD_MOBILENET;
        }
    } else {
        fprintf(stderr, "Unrecognized model (%d outputs), treating as SSD\n", num_outputs);
        model_type_ = ModelType::SSD_MOBILENET;
    }

    return true;
}

std::vector<Detection> Inference::run(const uint8_t* input_data, int input_size,
                                      float conf_threshold) {
    auto* input_tensor = impl_->interpreter->input_tensor(0);

    if (input_is_float_) {
        // Normalize uint8 [0,255] → float32 [0,1] directly into tensor buffer
        float* dst = input_tensor->data.f;
        for (int i = 0; i < input_w_ * input_h_ * 3; i++) {
            dst[i] = input_data[i] / 255.0f;
        }
    } else {
        memcpy(input_tensor->data.uint8, input_data, input_size);
    }

    if (impl_->interpreter->Invoke() != kTfLiteOk) {
        fprintf(stderr, "Invoke failed\n");
        return {};
    }

    if (model_type_ == ModelType::YOLOV8) {
        return parse_yolov8_output(conf_threshold);
    }
    return parse_ssd_output(conf_threshold);
}

// ── SSD MobileNet v2 parser ──
// Outputs: boxes[1,N,4], classes[1,N], scores[1,N], count[1]  (post-NMS from model)
// Box coords: [ymin, xmin, ymax, xmax] normalized [0,1]
std::vector<Detection> Inference::parse_ssd_output(float conf_threshold) {
    const float* boxes   = impl_->interpreter->output_tensor(0)->data.f;
    const float* classes = impl_->interpreter->output_tensor(1)->data.f;
    const float* scores  = impl_->interpreter->output_tensor(2)->data.f;
    int count = (int)impl_->interpreter->output_tensor(3)->data.f[0];

    std::vector<Detection> results;
    for (int i = 0; i < count; i++) {
        if (scores[i] < conf_threshold) continue;
        Detection d;
        d.y_min = boxes[i * 4 + 0];
        d.x_min = boxes[i * 4 + 1];
        d.y_max = boxes[i * 4 + 2];
        d.x_max = boxes[i * 4 + 3];
        d.class_id  = (int)classes[i];
        d.confidence = scores[i];
        results.push_back(d);
    }
    return results;
}

// ── YOLOv8 parser ──
// Output: [1, (4 + num_classes), num_anchors]
// Coords: cx, cy, w, h in pixel space [0, input_size]; normalize to [0,1]
// Scores: already in [0,1] (sigmoid applied by model)
// Supports float32 and int8 output tensors.
std::vector<Detection> Inference::parse_yolov8_output(float conf_threshold) {
    auto* out = impl_->interpreter->output_tensor(0);
    int feat_dim    = out->dims->data[1];  // 4 + num_classes
    int num_anchors = out->dims->data[2];
    int num_classes = feat_dim - 4;

    // Helper: dequantize output value at flat index
    auto get_val = [&](int idx) -> float {
        if (out->type == kTfLiteFloat32) {
            return out->data.f[idx];
        } else {  // kTfLiteInt8
            float scale      = out->params.scale;
            int32_t zp       = out->params.zero_point;
            return (out->data.int8[idx] - zp) * scale;
        }
    };

    std::vector<Detection> results;
    for (int a = 0; a < num_anchors; a++) {
        // Find best class score
        float best_score = 0.0f;
        int   best_class = 0;
        for (int c = 0; c < num_classes; c++) {
            float s = get_val((4 + c) * num_anchors + a);
            if (s > best_score) { best_score = s; best_class = c; }
        }
        if (best_score < conf_threshold) continue;

        // onnx2tf normalizes box coords to [0,1] — do NOT divide by input size again
        float cx = get_val(0 * num_anchors + a);
        float cy = get_val(1 * num_anchors + a);
        float w  = get_val(2 * num_anchors + a);
        float h  = get_val(3 * num_anchors + a);

        Detection d;
        d.x_min = cx - w * 0.5f;
        d.y_min = cy - h * 0.5f;
        d.x_max = cx + w * 0.5f;
        d.y_max = cy + h * 0.5f;
        d.class_id   = best_class;
        d.confidence = best_score;
        results.push_back(d);
    }
    return results;
}
