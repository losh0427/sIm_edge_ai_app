#include "preprocess.h"
#include <opencv2/opencv.hpp>

namespace preprocess {

std::vector<uint8_t> resize(const Frame& frame, int target_w, int target_h) {
    cv::Mat src(frame.height, frame.width, CV_8UC3, const_cast<uint8_t*>(frame.data.data()));
    cv::Mat dst;
    cv::resize(src, dst, cv::Size(target_w, target_h), 0, 0, cv::INTER_LINEAR);
    return std::vector<uint8_t>(dst.data, dst.data + dst.total() * dst.elemSize());
}

std::vector<uint8_t> encode_thumbnail(const Frame& frame, int max_w, int quality) {
    cv::Mat src(frame.height, frame.width, CV_8UC3, const_cast<uint8_t*>(frame.data.data()));
    cv::Mat bgr;
    cv::cvtColor(src, bgr, cv::COLOR_RGB2BGR);  // OpenCV imencode expects BGR
    // Aspect-ratio preserving resize: scale to max_w, compute height proportionally
    int thumb_w = (src.cols <= max_w) ? src.cols : max_w;
    int thumb_h = thumb_w * src.rows / src.cols;
    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(thumb_w, thumb_h), 0, 0, cv::INTER_AREA);
    std::vector<uint8_t> buf;
    cv::imencode(".jpg", resized, buf, {cv::IMWRITE_JPEG_QUALITY, quality});
    return buf;
}

void resize_into(const Frame& frame, int target_w, int target_h, uint8_t* out_buf, int out_size) {
    cv::Mat src(frame.height, frame.width, CV_8UC3, const_cast<uint8_t*>(frame.data.data()));
    // Wrap out_buf as a cv::Mat so resize writes directly into it
    cv::Mat dst(target_h, target_w, CV_8UC3, out_buf);
    cv::resize(src, dst, cv::Size(target_w, target_h), 0, 0, cv::INTER_LINEAR);
    (void)out_size;  // used for documentation; dst size must match
}

std::vector<uint8_t> encode_thumbnail_raw(const uint8_t* rgb_data, int w, int h, int channels,
                                          int max_w, int quality) {
    cv::Mat src(h, w, CV_8UC3, const_cast<uint8_t*>(rgb_data));
    cv::Mat bgr;
    cv::cvtColor(src, bgr, cv::COLOR_RGB2BGR);
    // Aspect-ratio preserving resize
    int thumb_w = (w <= max_w) ? w : max_w;
    int thumb_h = thumb_w * h / w;
    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(thumb_w, thumb_h), 0, 0, cv::INTER_AREA);
    std::vector<uint8_t> buf;
    cv::imencode(".jpg", resized, buf, {cv::IMWRITE_JPEG_QUALITY, quality});
    return buf;
}

}
