// DmaHeapAllocator —— IBufferAllocator 板端实现（任务分解 T3.x，详设 §2.1）
// /dev/dma_heap/system + DMA_HEAP_IOCTL_ALLOC + mmap：
//   分配的 buffer 有真实 dma-buf fd（MPP/RGA 可直通）与 CPU 映射（兜底 memcpy 路径）
// heap fd 进程内打开一次复用；分配失败返回 fd=-1 的空 Slab（调用方计 drop）
#pragma once

#include "common/FramePool.h"

namespace aibox {

class DmaHeapAllocator : public IBufferAllocator {
public:
    DmaHeapAllocator();
    ~DmaHeapAllocator() override;

    // fmt：kDrmFmtNV12 → w*h*3/2 字节；其他按 4 字节/像素兜底
    Slab allocate(uint32_t w, uint32_t h, uint32_t fmt) override;
    void deallocate(const Slab& s) override;

private:
    int heap_fd_ = -1;  // /dev/dma_heap/system（打开一次，进程内复用）
};

}  // namespace aibox
