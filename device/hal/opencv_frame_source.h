#pragma once
#include "i_frame_source.h"
#include <opencv2/videoio.hpp>
#include <string>

class OpenCVFrameSource : public IFrameSource {
public:
    OpenCVFrameSource(int cam_index = 0, int target_w = 640, int target_h = 480);
    bool open() override;
    bool next_frame(Frame& frame) override;
    void close() override;
    std::string describe() const override;

private:
    int cam_index_;
    int target_w_, target_h_;
    cv::VideoCapture cap_;
};
