// T1.2 单测（任务分解 §4-T1.2）
// 1) push 满 4 再 push —— 最旧被弹、drop=1、新帧在队尾
// 2) pop 超时 100ms —— 返回 false 不阻塞
// 3) drain 语义 —— pop 到空，meter 一致
// 4) SPSC 并发 —— 队列级不变量（静息精确）
#include <atomic>
#include <chrono>
#include <thread>

#include "common/BoundedQueue.h"
#include "test_util.h"

using namespace aibox;

namespace {

void run() {
    // --- 满弹最旧 ---
    {
        BoundedQueue<int> q;
        for (int i = 0; i < 4; ++i) {
            CHECK(!q.push_drop_oldest(std::move(i)));
        }
        CHECK(q.push_drop_oldest(std::move(100)));  // 第 5 个 → 弹最旧 0
        CHECK(q.meter().in.load() == 5);
        CHECK(q.meter().drop.load() == 1);
        CHECK(q.size() == 4);

        int v = -1;
        const int expect_seq[4] = {1, 2, 3, 100};  // 0 已被第 5 次 push 挤掉
        for (int i = 0; i < 4; ++i) {
            CHECK(q.pop(v, std::chrono::milliseconds(10)));
            CHECK(v == expect_seq[i]);
        }
        CHECK(q.empty());
        CHECK(q.meter().out.load() == 4);
    }

    // --- pop 超时 ---
    {
        BoundedQueue<int> q;
        const auto t0 = std::chrono::steady_clock::now();
        int v;
        CHECK(!q.pop(v, std::chrono::milliseconds(100)));
        const auto el = std::chrono::steady_clock::now() - t0;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(el).count();
        CHECK(ms >= 95 && ms < 300);  // 约 100ms，不永久阻塞
    }

    // --- drain ---
    {
        BoundedQueue<int> q;
        for (int i = 0; i < 3; ++i) q.push_drop_oldest(std::move(i));
        CHECK(q.drain() == 3);
        CHECK(q.empty());
        CHECK(q.drain() == 0);
        CHECK(q.meter().in.load() == 3 && q.meter().out.load() == 3 &&
              q.meter().drop.load() == 0);
    }

    // --- SPSC 并发 + 不变量 ---
    {
        BoundedQueue<uint64_t> q;
        constexpr int N = 100000;
        std::atomic<bool> done{false};
        std::thread prod([&q, &done] {
            for (int i = 0; i < N; ++i) {
                q.push_drop_oldest(std::move(uint64_t(i)));
            }
            done.store(true);
        });
        int got = 0;
        uint64_t v = 0;
        while (!done.load()) {  // 快生产慢消费 → 必有 drop
            if (q.pop(v, std::chrono::milliseconds(1))) ++got;
        }
        prod.join();
        while (q.pop(v, std::chrono::milliseconds(0))) ++got;  // 排空

        const uint64_t in = q.meter().in.load(), out = q.meter().out.load(),
                       drop = q.meter().drop.load();
        CHECK(in == static_cast<uint64_t>(N));
        CHECK(static_cast<uint64_t>(got) == out);        // 消费数 = out
        CHECK(in == out + drop);                         // 守恒：无泄漏
    }
}

}  // namespace

TEST_MAIN("test_bounded_queue")
