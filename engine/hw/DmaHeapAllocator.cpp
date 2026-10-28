#include "hw/DmaHeapAllocator.h"

#include "common/Log.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#if __has_include(<linux/dma-heap.h>)
#include <linux/dma-heap.h>
#else
// 老内核/x86 开发机无该 UAPI 头：本地定义等价结构（UAPI 自 5.6 起稳定）
#include <linux/ioctl.h>
#include <linux/types.h>
struct dma_heap_allocation_data {
    __u64 len;
    __u32 fd;
    __u32 fd_flags;
    __u64 heap_flags;
};
#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC \
    _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)
#endif

#include "common/DmaBuffer.h"

namespace aibox {

DmaHeapAllocator::DmaHeapAllocator() {
    heap_fd_ = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    if (heap_fd_ < 0)
        LOGE("dmaheap", "open /dev/dma_heap/system fail: %m");
}

DmaHeapAllocator::~DmaHeapAllocator() {
    if (heap_fd_ >= 0) close(heap_fd_);
}

Slab DmaHeapAllocator::allocate(uint32_t w, uint32_t h, uint32_t fmt) {
    Slab s;
    if (heap_fd_ < 0 || w == 0 || h == 0) return s;

    // NV12 = Y(w*h) + UV(w*h/2)；非 NV12 兜底按 4B/px（当前链路只用 NV12）
    size_t bytes = (fmt == kDrmFmtNV12)
                       ? static_cast<size_t>(w) * h * 3 / 2
                       : static_cast<size_t>(w) * h * 4;

    dma_heap_allocation_data d{};
    d.len = bytes;
    d.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap_fd_, DMA_HEAP_IOCTL_ALLOC, &d) < 0) {
        LOGE("dmaheap", "ioctl alloc %zu bytes fail: %m", bytes);
        return s;
    }
    s.fd = static_cast<int>(d.fd);
    s.bytes = bytes;
    s.map = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, s.fd, 0);
    if (s.map == MAP_FAILED) {
        LOGE("dmaheap", "mmap %zu bytes fail: %m", bytes);
        s.map = nullptr;
        close(s.fd);
        s.fd = -1;
        s.bytes = 0;
    }
    return s;
}

void DmaHeapAllocator::deallocate(const Slab& s) {
    if (s.map) munmap(const_cast<void*>(s.map), s.bytes);
    if (s.fd >= 0) close(s.fd);
}

}  // namespace aibox
