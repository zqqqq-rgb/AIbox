// T1.3 单测（任务分解 §4-T1.3）
// 1) 并发 10 万次 acquire/随机延迟回收 —— 结束 in_use()==0（无泄漏），fd 不重复
// 2) 池空 acquire —— 返回 nullptr（不等待）
#include <atomic>
#include <set>
#include <thread>
#include <vector>

#include "common/FramePool.h"
#include "test_util.h"

using namespace aibox;

namespace {

void run() {
    // --- 池空语义 ---
    {
        FramePool pool(4, 640, 480, kDrmFmtNV12);
        CHECK(pool.capacity() == 4);
        std::vector<std::shared_ptr<DmaBuffer>> held;
        for (int i = 0; i < 4; ++i) {
            auto b = pool.acquire();
            CHECK(b != nullptr);
            held.push_back(b);
        }
        CHECK(pool.in_use() == 4);
        auto none = pool.acquire();  // 第 5 次 → nullptr，立即返回
        CHECK(none == nullptr);
        held.clear();
        CHECK(pool.in_use() == 0);
        auto again = pool.acquire();  // 归还后可再取
        CHECK(again != nullptr);
        // fd 在途唯一：新取的 fd 与已释放的复用是设计行为，与在途的不冲突
        CHECK(again->w == 640 && again->h == 480 && again->map != nullptr);
    }

    // --- 并发 10 万次 + 随机延迟回收（池深 2 < 线程 4 → 必然踩到池空 nullptr 路径） ---
    {
        FramePool pool(2, 640, 480, kDrmFmtNV12);
        std::atomic<int> total_acquired{0};
        std::atomic<int> pool_empty{0};
        std::atomic<bool> fail{false};
        std::vector<std::thread> workers;
        for (int t = 0; t < 4; ++t) {
            workers.emplace_back([&pool, &total_acquired, &pool_empty, &fail] {
                uint64_t rng = 0x9e3779b97f4a7c15ull ^ (uintptr_t)&fail;
                for (int i = 0; i < 25000; ++i) {
                    auto b = pool.acquire();
                    if (b == nullptr) {
                        pool_empty.fetch_add(1, std::memory_order_relaxed);
                        continue;  // 池空是设计行为，立即返回不等待
                    }
                    total_acquired.fetch_add(1, std::memory_order_relaxed);
                    if (pool.in_use() > 2) fail.store(true);  // 超发即错
                    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                    if ((rng % 16) == 0) {  // 偶发持有延迟（控制总时长）
                        std::this_thread::yield();
                    }
                    // shared_ptr 析构 → 回池
                }
            });
        }
        for (auto& th : workers) th.join();
        CHECK(!fail.load());
        CHECK(pool.in_use() == 0);  // 无泄漏：全部归还
        CHECK(total_acquired.load() + pool_empty.load() == 100000);
        CHECK(total_acquired.load() > 0);
    }

    // --- 在途 fd 唯一性（模拟 dma-buf fd 语义） ---
    {
        FramePool pool(4, 64, 48, kDrmFmtNV12);
        std::set<int> fds;
        std::vector<std::shared_ptr<DmaBuffer>> held;
        while (auto b = pool.acquire()) {
            CHECK(fds.insert(b->fd).second);  // 在途 fd 不得重复
            held.push_back(b);
        }
        CHECK(fds.size() == 4);
    }
}

}  // namespace

TEST_MAIN("test_frame_pool")
