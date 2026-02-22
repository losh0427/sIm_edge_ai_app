#pragma once
#include <cstdint>
#include <vector>
#include <string>

struct Frame {
    std::vector<uint8_t> data;  // RGB pixels
    int width;
    int height;
    int channels;  // always 3
};

class IFrameSource {
public:
    virtual ~IFrameSource() = default;
    virtual bool open() = 0;
    virtual bool next_frame(Frame& frame) = 0;
    virtual void close() = 0;
    virtual std::string describe() const = 0;
};
