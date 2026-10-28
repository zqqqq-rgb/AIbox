// NpuCorePlan 单测（NPU 核分配配置面：parse/valid/default_for/auto_detect/fallback）
// 纯逻辑零依赖，x86 CI 毫秒级；板上跑用例 5 时走真实 SoC 默认表分支（两种均须合法）
#include <string>

#include "infer/NpuCorePlan.h"
#include "test_util.h"

using namespace aibox;

int main() {
    // ---- 1. parse + valid：合法计划（3 核默认表） ----
    {
        NpuCorePlan p;
        CHECK(NpuCorePlan::parse("steady=0,1:trig=2", p));
        CHECK(p.valid());
        CHECK(p.steady_n() == 2 && p.triggered_n() == 1);
        CHECK(p.steady_cores[0] == 0 && p.steady_cores[1] == 1 && p.triggered_cores[0] == 2);
    }

    // ---- 2. valid：非法形态逐一拒绝 ----
    {
        NpuCorePlan p{{}, {1}};
        std::string err;
        CHECK(!p.valid(&err));  // steady 空
    }
    {
        NpuCorePlan p{{0}, {}};
        CHECK(!p.valid());  // triggered 空
    }
    {
        NpuCorePlan p{{0, 3}, {1}};
        CHECK(!p.valid());  // 核 id 越界（仅 0~2）
    }
    {
        NpuCorePlan p{{0, 0}, {1}};
        CHECK(!p.valid());  // 核重复分配
    }
    {
        NpuCorePlan p{{0, 1}, {1, 2}};
        CHECK(!p.valid());  // 两队列相交（分区纪律）
    }

    // ---- 3. default_for：2/3 核默认表；其余核数拒绝 ----
    {
        NpuCorePlan p;
        CHECK(NpuCorePlan::default_for(2, p));
        CHECK(p.steady_n() == 1 && p.triggered_n() == 1);
        CHECK(p.steady_cores[0] == 0 && p.triggered_cores[0] == 1);
        CHECK(NpuCorePlan::default_for(3, p));
        CHECK(p.steady_n() == 2 && p.triggered_n() == 1);
        CHECK(p.triggered_cores[0] == 2);
        std::string err;
        CHECK(!NpuCorePlan::default_for(1, p, &err));
        CHECK(!NpuCorePlan::default_for(4, p, &err));
    }

    // ---- 4. parse：别名/非法输入返回 false 且 out 不被改写 ----
    {
        NpuCorePlan q{{9}, {9}};  // 哨兵：失败路径不得动 out
        CHECK(!NpuCorePlan::parse("steady=0", q));           // 缺 trig
        CHECK(!NpuCorePlan::parse("steady=0:trig=3", q));    // 核 id 越界
        CHECK(!NpuCorePlan::parse("foo=0:trig=1", q));       // 未知队列名
        CHECK(!NpuCorePlan::parse("steady=:trig=1", q));     // 空核列表
        CHECK(!NpuCorePlan::parse("steady=0,1:trig=1", q));  // 两队列相交
        CHECK(q.steady_cores[0] == 9 && q.triggered_cores[0] == 9);
        CHECK(NpuCorePlan::parse("st=0:trig=1,2", q));  // 别名 st/trig
        CHECK(q.steady_n() == 1 && q.triggered_n() == 2);
    }

    // ---- 5. auto_detect：返回合法计划（x86 无 device-tree → 未知 SoC 兜底 2 核；
    //         板上跑则为 rk3588/rk3576 真实默认表——两种形态均须通过）----
    {
        std::string note;
        NpuCorePlan p = NpuCorePlan::auto_detect(&note);
        CHECK(p.valid());
        const bool two = p.steady_n() == 1 && p.triggered_n() == 1;
        const bool three = p.steady_n() == 2 && p.triggered_n() == 1;
        CHECK(two || three);
        // fallback：历史双核行为（校验失败时的调用方兜底）
        NpuCorePlan f = NpuCorePlan::fallback();
        CHECK(f.steady_cores[0] == 0 && f.triggered_cores[0] == 1);
    }

    if (g_fail == 0) printf("npu_core_plan PASSED\n");
    return g_fail;
}
