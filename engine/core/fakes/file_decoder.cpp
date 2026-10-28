#include "core/fakes/file_decoder.h"

#include <cstddef>

namespace aibox {

bool FileDecoder::open(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || width % 2 != 0 || height % 2 != 0) {
        return false;  // NV12 要求宽高为偶数
    }
    width_ = width;
    height_ = height;
    frame_index_ = 0;
    return true;
}

bool FileDecoder::decode(const Packet& in, Frame& out) {
    if (in.data == nullptr || in.size != nv12_size(width_, height_)) {
        return false;  // 测试资产每包必须恰好一帧
    }
    out = Frame{};
    out.pts_ms = in.pts_ms;
    out.width = width_;
    out.height = height_;
    out.format = PixelFormat::kNV12;
    out.stride_y = width_;
    out.stride_uv = width_;
    out.y_ptr = in.data;
    out.uv_ptr = in.data + static_cast<size_t>(width_) * height_;
    out.frame_index = frame_index_++;
    return true;
}

void FileDecoder::close() {
    width_ = height_ = 0;
}

}  // namespace aibox
