#include "common/FramePool.h"

#include <cstdlib>
#include <new>

namespace aibox {

namespace {

// x86 模拟分配器：普通内存 + 合成 fd（全局递增，保证在途 buffer fd 唯一）
class MallocAllocator : public IBufferAllocator {
public:
    Slab allocate(uint32_t w, uint32_t h, uint32_t fmt) override {
        (void)fmt;  // M1 仅 NV12；其他格式随 M3 RGA 路径补
        const size_t bytes = static_cast<size_t>(w) * h * 3 / 2;
        void* p = nullptr;
        if (posix_memalign(&p, 64, bytes) != 0) {
            throw std::bad_alloc();
        }
        Slab s;
        s.map = p;
        s.fd = next_fd_.fetch_add(1, std::memory_order_relaxed);
        s.bytes = bytes;
        return s;
    }

    void deallocate(const Slab& s) override { free(s.map); }

private:
    std::atomic<int> next_fd_{1};
};

}  // namespace

// 共享 State：池析构后，在途 Job 的 buffer deleter 仍持有本对象——
// slab 内存与 shell 存活至全部引用归零（remove_cam 清理期 UAF 的修复，压测 v2 实证）
struct FramePool::State {
    IBufferAllocator* alloc = nullptr;
    bool owns_alloc = false;
    std::vector<Slab> slabs;
    std::vector<std::unique_ptr<DmaBuffer>> shells;  // 预分配，运行期零结构分配
    std::atomic<uint64_t> free_mask{0};
    std::atomic<size_t> in_use{0};

    void release_slot(size_t idx) {
        in_use.fetch_sub(1, std::memory_order_relaxed);
        free_mask.fetch_or(1ULL << idx, std::memory_order_release);
    }

    ~State() {
        for (const auto& slab : slabs) {
            alloc->deallocate(slab);
        }
        if (owns_alloc) {
            delete alloc;
        }
    }
};

FramePool::FramePool(size_t n, uint32_t w, uint32_t h, uint32_t fmt,
                     IBufferAllocator* alloc) {
    // 位图空闲链上限 64（AI 池 8 / 显示池 4，16 通道各建独立池，远小于上限）
    if (n == 0 || n > 64) {
        std::abort();
    }
    state_ = std::make_shared<State>();
    state_->alloc = alloc;
    if (state_->alloc == nullptr) {
        state_->alloc = new MallocAllocator();
        state_->owns_alloc = true;
    }

    state_->free_mask.store(n == 64 ? ~0ULL : (1ULL << n) - 1,
                            std::memory_order_relaxed);

    state_->slabs.reserve(n);
    state_->shells.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        state_->slabs.push_back(state_->alloc->allocate(w, h, fmt));

        auto shell = std::make_unique<DmaBuffer>();
        shell->fd = state_->slabs.back().fd;
        shell->w = w;
        shell->h = h;
        shell->fmt = fmt;
        shell->map = state_->slabs.back().map;
        state_->shells.push_back(std::move(shell));
    }
}

FramePool::~FramePool() = default;  // 仅释放自身 State 引用（在途 buffer 延迟归还）

std::shared_ptr<DmaBuffer> FramePool::acquire() {
    State& st = *state_;
    uint64_t m = st.free_mask.load(std::memory_order_acquire);
    for (;;) {
        if (m == 0) {
            return nullptr;  // 池空：设计行为，调用方计 drop
        }
        const unsigned idx = __builtin_ctzll(m);
        const uint64_t bit = 1ULL << idx;
        if (st.free_mask.compare_exchange_weak(m, m & ~bit, std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
            st.in_use.fetch_add(1, std::memory_order_relaxed);
            DmaBuffer* raw = st.shells[idx].get();
            // deleter 按值持有 State 强引用：buffer 引用即 State 引用——
            // 池析构/通道拆除后，在途 Job 使用期间 slab 与 shell 保证存活。
            // 捕获局部副本 keep（成员 state_ 的捕获会隐式拖入 this，
            // 而 deleter 可能在池析构后执行——届时悬垂 this 即 UAF）
            auto keep = state_;
            return std::shared_ptr<DmaBuffer>(raw, [keep, idx](DmaBuffer*) {
                keep->release_slot(idx);
            });
        }
    }
}

size_t FramePool::in_use() const { return state_->in_use.load(std::memory_order_relaxed); }

size_t FramePool::capacity() const { return state_->shells.size(); }

}  // namespace aibox
