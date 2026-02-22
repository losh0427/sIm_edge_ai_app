#include "file_frame_source.h"
#include <opencv2/opencv.hpp>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

FileFrameSource::FileFrameSource(const std::string& dir, int target_w, int target_h)
    : dir_(dir), target_w_(target_w), target_h_(target_h) {}

bool FileFrameSource::open() {
    for (auto& entry : fs::directory_iterator(dir_)) {
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png")
            files_.push_back(entry.path().string());
    }
    std::sort(files_.begin(), files_.end());
    return !files_.empty();
}

bool FileFrameSource::next_frame(Frame& frame) {
    if (files_.empty()) return false;
    cv::Mat img = cv::imread(files_[idx_ % files_.size()]);
    if (img.empty()) return false;
    idx_++;

    cv::Mat resized;
    cv::resize(img, resized, cv::Size(target_w_, target_h_));
    cv::cvtColor(resized, resized, cv::COLOR_BGR2RGB);

    frame.width = target_w_;
    frame.height = target_h_;
    frame.channels = 3;
    frame.data.assign(resized.data, resized.data + resized.total() * resized.elemSize());
    return true;
}

void FileFrameSource::close() { files_.clear(); idx_ = 0; }

std::string FileFrameSource::describe() const {
    return "FILE:" + dir_ + ":" + std::to_string(target_w_) + "x" + std::to_string(target_h_);
}
