// FramePool —— 预分配帧池（任务分解 T1.3，详设 §2.1）
// 进程启动预分配，运行期零大块分配；池深即背压内存上界
//   （16 通道 × 8 × 640×480×1.5B ≈ 56MB，dma-buf 计入 CMA 不计 RSS）
// acquire() 池空返回 nullptr 是设计行为：下游积压 → 调用方丢帧 + EdgeMeter.drop++
//
// 分配器接缝：x86 用内置 MallocAllocator 模拟 dma-buf（fd 为合成句柄），
// 板上换 DmaHeapAllocator（/dev/dma_heap/system，M3 hw 层实现）——池逻辑本身在 core
//
// 空闲链实现：64 位位图 CAS（无锁栈的 ABA 隐患为零，池深 ≤64 恒成立）
// 详设描述为"无锁栈"，位图等价且更简单：acquire=清最低置位，release=置位
//
// 生命周期（压测 v2 板上实证修正）：原约定"池必须 outlive 在途 buffer（CameraChannel
// 保证）"被调度路径打破——remove_cam 析构池时，NpuScheduler 在途 Job 仍持有 buffer
// 引用 → UAF（RGA 参数变垃圾，preproc 防御拦下未崩，实为未定义行为）。
// 修正：slab/shell 所有权入共享 State，buffer 的 deleter 持 State 引用——
// 内存归还延迟到"池自身 + 全部在途 buffer 引用归零"，析构顺序不再构成约束。
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include "common/DmaBuffer.h"
#include "common/NonCopyable.h"

namespace aibox {

struct Slab {
    void* map = nullptr;
    int fd = -1;
    size_t bytes = 0;
};

// 分配器接缝：板上实现放 hw 层（dma_heap/DRM dumb fallback，详设 §2.1）
struct IBufferAllocator {
    virtual ~IBufferAllocator() = default;
    virtual Slab allocate(uint32_t w, uint32_t h, uint32_t fmt) = 0;
    virtual void deallocate(const Slab& s) = 0;
};

class FramePool : public NonCopyable {
public:
    // n: 池深（子码流 AI 池 = 8 = RGA+NPU 在途上限；显示池 = 4）
    // alloc 为空时使用内置 x86 模拟分配器（malloc + 合成 fd），板上显式传入
    FramePool(size_t n, uint32_t w, uint32_t h, uint32_t fmt,
              IBufferAllocator* alloc = nullptr);
    // 仅释放池自身持有的 State 引用；slab 内存随最后一个在途 buffer 释放而归还
    //（Allocator 指针由调用方保证 outlive State——main.cpp 的 App 成员序满足）
    ~FramePool();

    // 非阻塞获取；池空返回 nullptr（调用方计 drop，绝不等待）
    std::shared_ptr<DmaBuffer> acquire();

    size_t in_use() const;
    size_t capacity() const;

private:
    struct State;                      // slab/shell/空闲位图（见 .cpp）
    std::shared_ptr<State> state_;     // 与在途 buffer 共享（deleter 持引用）
};

}  // namespace aibox
