// FileSource —— 测试用拉流源（ISource 的 x86 实现）
// 从文件按固定块读出 Packet：块大小=帧大小 → 每 Packet 对应一帧，
// pts_ms = frame_index * frame_interval_ms（归零透传，与 pts 对齐锚定语义一致）
#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "interfaces/isource.h"

namespace aibox {

class FileSource : public ISource {
public:
    // chunk_size：每包字节数（测试资产 = NV12 帧大小）；frame_interval_ms：仿真帧率
    FileSource(std::string path, uint32_t chunk_size, uint32_t frame_interval_ms,
               bool loop = false);

    bool open() override;
    bool read(Packet& out) override;
    void close() override;

private:
    std::string path_;
    uint32_t chunk_size_;
    uint32_t frame_interval_ms_;
    bool loop_;
    std::ifstream file_;
    std::vector<uint8_t> buf_;  // Packet data 指向此处（借用语义）
    uint64_t frame_index_ = 0;
};

}  // namespace aibox
