// BoundedQueue —— SPSC 有界队列（任务分解 T1.2，详设 §2.2，架构铁律 2 的载体）
// 满则弹最旧（newest-wins）：AI 路径宁可丢旧帧，绝不阻塞生产者（解码回调线程）
// SPSC 假设由线程全景表（§1.4）保证；跨生产者场景拆两个队列
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <utility>

#include "common/EdgeMeter.h"
#include "common/NonCopyable.h"

namespace aibox {

template <typename T, size_t Cap = 4>
class BoundedQueue : public NonCopyable {
    static_assert(Cap >= 1, "Cap >= 1");

public:
    // 满则弹最旧并计 drop；返回是否发生丢弃
    // 语义：in 计每次 push 尝试（含被挤掉的），故不变量 in==out+drop+深度 恒成立
    bool push_drop_oldest(T&& v) {
        bool dropped = false;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (size_ == Cap) {
                head_ = (head_ + 1) % Cap;  // 弹最旧
                --size_;
                dropped = true;
                meter_.drop.fetch_add(1, std::memory_order_relaxed);
            }
            ring_[(head_ + size_) % Cap] = std::move(v);
            ++size_;
            meter_.in.fetch_add(1, std::memory_order_relaxed);
        }
        cv_.notify_one();
        return dropped;
    }

    // 带超时 pop；超时返回 false（线程优雅退出轮询）；成功计 out
    bool pop(T& out, std::chrono::milliseconds timeout) {
        {
            std::unique_lock<std::mutex> lk(m_);
            if (!cv_.wait_for(lk, timeout, [this] { return size_ > 0; })) {
                return false;
            }
            out = std::move(ring_[head_]);
            head_ = (head_ + 1) % Cap;
            --size_;
            meter_.out.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }

    // pop 到空（进程优雅退出收尾用）；返回排出帧数（均计 out）
    size_t drain() {
        size_t n = 0;
        T sink;
        while (pop(sink, std::chrono::milliseconds(0))) {
            ++n;
        }
        return n;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(m_);
        return size_;
    }
    bool empty() const { return size() == 0; }

    const EdgeMeter& meter() const { return meter_; }

private:
    std::array<T, Cap> ring_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    size_t head_ = 0, size_ = 0;
    EdgeMeter meter_;
};

}  // namespace aibox
