#include "core/fakes/file_source.h"

#include <chrono>
#include <thread>

namespace aibox {

FileSource::FileSource(std::string path, uint32_t chunk_size, uint32_t frame_interval_ms,
                       bool loop)
    : path_(std::move(path)),
      chunk_size_(chunk_size),
      frame_interval_ms_(frame_interval_ms),
      loop_(loop) {}

bool FileSource::open() {
    file_.open(path_, std::ios::binary);
    if (!file_.is_open() || chunk_size_ == 0) {
        return false;
    }
    buf_.resize(chunk_size_);
    frame_index_ = 0;
    return true;
}

bool FileSource::read(Packet& out) {
    if (!file_.is_open()) {
        return false;
    }
    file_.read(reinterpret_cast<char*>(buf_.data()), static_cast<std::streamsize>(chunk_size_));
    const auto got = static_cast<uint32_t>(file_.gcount());
    if (got == 0) {
        if (loop_) {  // 循环模式：重开文件继续读（长稳测试用）
            file_.clear();
            file_.seekg(0);
            frame_index_ = 0;
            return read(out);
        }
        return false;  // EOF：由调用方决定重连节奏
    }
    out.data = buf_.data();
    out.size = got;
    out.pts_ms = frame_index_ * frame_interval_ms_;
    ++frame_index_;
    // T0.3 节拍回放：按帧间隔睡眠（0=不限速）。无节拍时裸读文件全速循环——
    // Sampler 按墙钟采样到的 frame_pts 伪随机乱跳，且 CPU 空转（板上实测踩中）
    if (frame_interval_ms_ > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(frame_interval_ms_));
    return true;
}

void FileSource::close() {
    if (file_.is_open()) {
        file_.close();
    }
}

}  // namespace aibox
