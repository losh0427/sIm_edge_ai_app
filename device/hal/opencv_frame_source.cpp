#include "opencv_frame_source.h"
#include <opencv2/imgproc.hpp>
#include <cstdio>

OpenCVFrameSource::OpenCVFrameSource(int cam_index, int target_w, int target_h)
    : cam_index_(cam_index), target_w_(target_w), target_h_(target_h) {}

bool OpenCVFrameSource::open() {
    cap_.open(cam_index_, cv::CAP_V4L2);
    if (!cap_.isOpened()) {
        fprintf(stderr, "OpenCVFrameSource: failed to open /dev/video%d\n", cam_index_);
        return false;
    }

    // MJPEG format + small buffer for WSL2 usbipd (avoids select timeout)
    cap_.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap_.set(cv::CAP_PROP_FRAME_WIDTH, target_w_);
    cap_.set(cv::CAP_PROP_FRAME_HEIGHT, target_h_);
    cap_.set(cv::CAP_PROP_BUFFERSIZE, 1);

    fprintf(stderr, "OpenCVFrameSource: opened /dev/video%d (%dx%d MJPEG)\n",
            cam_index_, target_w_, target_h_);
    return true;
}

bool OpenCVFrameSource::next_frame(Frame& frame) {
    cv::Mat bgr;
    if (!cap_.read(bgr) || bgr.empty()) {
        fprintf(stderr, "OpenCVFrameSource: read failed\n");
        return false;
    }

    cv::Mat resized;
    if (bgr.cols != target_w_ || bgr.rows != target_h_)
        cv::resize(bgr, resized, cv::Size(target_w_, target_h_));
    else
        resized = bgr;

    cv::Mat rgb;
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);

    frame.width = target_w_;
    frame.height = target_h_;
    frame.channels = 3;
    frame.data.assign(rgb.data, rgb.data + rgb.total() * rgb.elemSize());
    return true;
}

void OpenCVFrameSource::close() {
    if (cap_.isOpened())
        cap_.release();
}

std::string OpenCVFrameSource::describe() const {
    return "OPENCV:cam" + std::to_string(cam_index_) + ":"
           + std::to_string(target_w_) + "x" + std::to_string(target_h_);
}
