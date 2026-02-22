#pragma once
#include "i_frame_source.h"
#include <string>
#include <vector>

class FileFrameSource : public IFrameSource {
public:
    FileFrameSource(const std::string& dir, int target_w = 640, int target_h = 480);
    bool open() override;
    bool next_frame(Frame& frame) override;
    void close() override;
    std::string describe() const override;

private:
    std::string dir_;
    int target_w_, target_h_;
    std::vector<std::string> files_;
    size_t idx_ = 0;
};
