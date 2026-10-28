// ISource —— 拉流源接缝（任务分解 T0.2）
// 实现：hw/RtspPuller（板上 ZLM SDK/ffmpeg 拉流）、core/fakes/FileSource（x86 测试）
#pragma once

#include "interfaces/types.h"

namespace aibox {

class ISource {
public:
    virtual ~ISource() = default;

    virtual bool open() = 0;

    // 阻塞读取一包；返回 false 表示 EOF/错误（调用方决定重连节奏，防双层重试风暴）
    virtual bool read(Packet& out) = 0;

    virtual void close() = 0;
};

}  // namespace aibox
