// T2.4 单测（任务分解 §4-T2.4）：多边形三态判定 + 防抖窗 + DWELL/STATE_SWITCH 语义
// 板上加固（事件接线排障）：边沿判定带 2 帧滞后确认 + 新 track 1s 冷静期——
// 跟踪 ID 抖动/ROI 边界抖动会把静止目标变成反复"进入"（板上实证 region_in
// 对静止目标 ~6-15s 误报一次）
// 板上加固 2（真实离开重现）：同 id 消失 ≥1.5s 回归重置 track 状态 + 冷静期
// 出生豁免（走入型/空窗后重现豁免，繁忙出生 pop-in 仍抑制）——板上实证
// "报警一次后离开画面再进 ROI 不再报警"，快照随事件缺失为连带现象
#include "rules/RuleEngine.h"
#include "test_util.h"

using namespace aibox;

namespace {

proto::DetTarget tgt(int32_t track, float x, float y, float w, float h, int32_t cls = 0) {
    proto::DetTarget t;
    t.track_id = track;
    t.x = x;
    t.y = y;
    t.w = w;
    t.h = h;
    t.cls = cls;
    t.conf = 0.9f;
    return t;
}

RuleEngine::Rule region_in(const std::vector<std::pair<float, float>>& poly,
                           std::string type = "intrusion", uint32_t debounce = 0) {
    RuleEngine::Rule r;
    r.type = RuleEngine::Type::REGION_IN;
    r.polygon = poly;
    r.event_type = std::move(type);
    r.debounce_sec = debounce;
    return r;
}

// 640×480 帧，中央矩形 ROI（归一化 0.25~0.75）
const std::vector<std::pair<float, float>> kBox = {
    {0.25f, 0.25f}, {0.75f, 0.25f}, {0.75f, 0.75f}, {0.25f, 0.75f}};

}  // namespace

int main() {
    // ---- 1. 多边形三态：内 / 外 / 边上（边界归属内） ----
    {
        bool on_edge = false;
        CHECK(point_in_polygon(0.5f, 0.5f, kBox, &on_edge) == true);   // 内
        CHECK(point_in_polygon(0.1f, 0.1f, kBox) == false);            // 外
        CHECK(point_in_polygon(0.25f, 0.5f, kBox, &on_edge) == true);  // 边上→内
        CHECK(on_edge);
        CHECK(point_in_polygon(0.75f, 0.75f, kBox) == true);  // 顶点→内
        CHECK(point_in_polygon(0.75f, 0.9f, kBox) == false);  // 顶点延长线→外
    }

    // ---- 2. REGION_IN/OUT 边沿（2 帧滞后 + 冷静期出生豁免语义） ----
    {
        RuleEngine re;
        re.set_frame_size(1, 640, 480);
        std::vector<RuleEngine::Rule> rules;
        rules.push_back(region_in(kBox, "intrusion"));
        RuleEngine::Rule ro;
        ro.type = RuleEngine::Type::REGION_OUT;
        ro.polygon = kBox;
        ro.event_type = "leave";
        rules.push_back(ro);
        re.set_rules(1, std::move(rules));

        // 走入型（首见在 ROI 外）：born 豁免冷静期 → 2 帧滞后后 FIRE
        CHECK(re.eval(1, {tgt(5, 10, 10, 50, 50)}, 0).empty());
        CHECK(re.eval(1, {tgt(5, 280, 200, 100, 100)}, 100).empty());
        auto ev = re.eval(1, {tgt(5, 280, 200, 100, 100)}, 200);
        CHECK(ev.size() == 1 && ev[0].event_type == "intrusion" && ev[0].ts_ms == 200);
        // 稳定在内：无事件
        CHECK(re.eval(1, {tgt(5, 290, 210, 100, 100)}, 1100).empty());

        // 真实离开（2 帧确认）→ REGION_OUT
        CHECK(re.eval(1, {tgt(5, 10, 10, 50, 50)}, 2000).empty());
        ev = re.eval(1, {tgt(5, 10, 10, 50, 50)}, 2100);
        CHECK(ev.size() == 1 && ev[0].event_type == "leave");

        // pop-in 型（首见在 ROI 内 + 场景繁忙）：冷静期仍抑制（track5 持续在外暖场）
        CHECK(re.eval(1, {tgt(5, 10, 10, 50, 50)}, 2200).empty());
        CHECK(re.eval(1, {tgt(6, 280, 200, 100, 100)}, 2300).empty());  // born内+busy 抑制
        CHECK(re.eval(1, {tgt(6, 280, 200, 100, 100)}, 2400).empty());  // 翻转 → fresh 抑制
        CHECK(re.eval(1, {tgt(6, 280, 200, 100, 100)}, 3500).empty());  // 稳定在内无事件
    }

    // ---- 3. 防抖：同类事件 5s 窗 → 仅 1 条，ts=首帧 ----
    {
        RuleEngine re;
        re.set_frame_size(1, 640, 480);
        std::vector<RuleEngine::Rule> rules;
        rules.push_back(region_in(kBox, "intrusion", 5));  // 5s 防抖
        re.set_rules(1, std::move(rules));

        // 建立轨迹：外→内（滞后 2 帧翻转），翻转帧在冷静期外 → 首次 FIRE
        std::vector<AlarmEvent> all;
        CHECK(re.eval(1, {tgt(7, 10, 10, 50, 50)}, 0).empty());
        CHECK(re.eval(1, {tgt(7, 280, 200, 100, 100)}, 100).empty());
        auto ev = re.eval(1, {tgt(7, 280, 200, 100, 100)}, 1300);
        CHECK(ev.size() == 1 && ev[0].ts_ms == 1300);
        all = ev;  // Include the first event before asserting the combined count.

        // 反复出/入（短暂离开 <2s）：防抖窗内合并；且冷却不被"短暂离开"解除
        for (int i = 0; i < 4; ++i) {
            const int64_t t0 = 2000 + i * 1200;
            re.eval(1, {tgt(7, 10, 10, 50, 50)}, t0);            // 出 pend1
            re.eval(1, {tgt(7, 10, 10, 50, 50)}, t0 + 100);      // 翻转出
            re.eval(1, {tgt(7, 280, 200, 100, 100)}, t0 + 200);  // 进 pend1（out 仅 100ms）
            auto e = re.eval(1, {tgt(7, 280, 200, 100, 100)}, t0 + 300);  // 翻转进
            for (auto& x : e) all.push_back(std::move(x));
        }
        CHECK(all.size() == 1);  // 4 次边沿全部被 防抖窗/冷却 吞掉
        if (!all.empty()) {
            CHECK(all[0].ts_ms == 1300);
            CHECK(all[0].event_type == "intrusion");
        }

        // 长间断（>1.5s）再入：走"真实离开重现"路径重置状态与冷却；
        // >5s 防抖窗外 → 正常上报
        re.eval(1, {tgt(7, 10, 10, 50, 50)}, 12000);   // 重现重置（gap 6.1s）；born外豁免
        re.eval(1, {tgt(7, 10, 10, 50, 50)}, 12100);   // 稳定在外
        re.eval(1, {tgt(7, 10, 10, 50, 50)}, 14600);   // 再次重现重置（gap 2.5s）
        re.eval(1, {tgt(7, 280, 200, 100, 100)}, 14700);  // 回内 pend1（冷却已清）
        ev = re.eval(1, {tgt(7, 280, 200, 100, 100)}, 14800);
        CHECK(ev.size() == 1 && ev[0].ts_ms == 14800);
    }

    // ---- 4. DWELL：驻留达标报一次，离开重新武装（计时锚定在滞后翻转帧） ----
    // （相邻 eval 间隔 <1.5s：更长间断会被"真实离开重现"重置状态，属用例 7 语义）
    {
        RuleEngine re;
        re.set_frame_size(1, 640, 480);
        std::vector<RuleEngine::Rule> rules;
        RuleEngine::Rule r;
        r.type = RuleEngine::Type::DWELL;
        r.polygon = kBox;
        r.event_type = "dwell";
        r.dwell_sec = 3;
        r.debounce_sec = 0;
        rules.push_back(r);
        re.set_rules(1, std::move(rules));

        CHECK(re.eval(1, {tgt(9, 10, 10, 50, 50)}, 0).empty());        // 外
        CHECK(re.eval(1, {tgt(9, 300, 300, 50, 50)}, 100).empty());    // pend1 未翻转
        CHECK(re.eval(1, {tgt(9, 305, 305, 50, 50)}, 200).empty());    // 翻转进 dwell_start=200
        CHECK(re.eval(1, {tgt(9, 310, 310, 50, 50)}, 1500).empty());   // 1300 <3000 未达标
        CHECK(re.eval(1, {tgt(9, 310, 310, 50, 50)}, 2800).empty());   // 2600 <3000 未达标
        auto ev = re.eval(1, {tgt(9, 310, 310, 50, 50)}, 3200);        // 3000 ≥3000 达标
        CHECK(ev.size() == 1 && ev[0].event_type == "dwell");
        CHECK(re.eval(1, {tgt(9, 320, 320, 50, 50)}, 3300).empty());   // 已报不重复

        CHECK(re.eval(1, {tgt(9, 10, 10, 50, 50)}, 3400).empty());     // 出 pend1（未翻转）
        CHECK(re.eval(1, {tgt(9, 10, 10, 50, 50)}, 3500).empty());     // 翻转出 → 重置/武装
        CHECK(re.eval(1, {tgt(9, 300, 300, 50, 50)}, 3600).empty());   // 重进 pend1
        CHECK(re.eval(1, {tgt(9, 300, 300, 50, 50)}, 3700).empty());   // 翻转进 dwell_start=3700
        CHECK(re.eval(1, {tgt(9, 300, 300, 50, 50)}, 5000).empty());   // 1300 未达标
        CHECK(re.eval(1, {tgt(9, 300, 300, 50, 50)}, 6300).empty());   // 2600 未达标
        ev = re.eval(1, {tgt(9, 300, 300, 50, 50)}, 6700);             // 3000 ≥3000 重新武装后达标
        CHECK(ev.size() == 1);
    }

    // ---- 5. STATE_SWITCH：同 track 类别变化上报新类别 ----
    {
        RuleEngine re;
        re.set_frame_size(1, 640, 480);
        std::vector<RuleEngine::Rule> rules;
        RuleEngine::Rule r;
        r.type = RuleEngine::Type::STATE_SWITCH;
        r.polygon = kBox;
        r.event_type = "helmet_change";
        rules.push_back(r);
        re.set_rules(1, std::move(rules));

        CHECK(re.eval(1, {tgt(3, 300, 300, 50, 50, /*cls=*/0)}, 0).empty());  // 首帧立状态
        auto ev = re.eval(1, {tgt(3, 300, 300, 50, 50, /*cls=*/1)}, 1000);    // 0→1
        CHECK(ev.size() == 1 && ev[0].targets[0].cls == 1);
        CHECK(re.eval(1, {tgt(3, 300, 300, 50, 50, /*cls=*/1)}, 2000).empty());
        ev = re.eval(1, {tgt(3, 300, 300, 50, 50, /*cls=*/0)}, 3000);         // 1→0
        CHECK(ev.size() == 1 && ev[0].targets[0].cls == 0);
    }

    // ---- 6. set_rules 全量替换清空 track 状态（RECONFIG 语义） ----
    {
        RuleEngine re;
        re.set_frame_size(1, 640, 480);
        std::vector<RuleEngine::Rule> rules;
        rules.push_back(region_in(kBox, "intrusion"));
        re.set_rules(1, std::move(rules));
        // 暖场（track99 在外）：使主目标 born-in-ROI 落入"繁忙出生" → 冷静期抑制，
        // 建立状态阶段不产生报警
        CHECK(re.eval(1, {tgt(99, 10, 10, 50, 50)}, 0).empty());
        // 建立状态：目标在内（滞后翻转发生在冷静期内，事件被抑制）
        CHECK(re.eval(1, {tgt(5, 280, 200, 100, 100)}, 100).empty());
        CHECK(re.eval(1, {tgt(5, 290, 210, 100, 100)}, 200).empty());
        CHECK(re.eval(1, {tgt(5, 290, 210, 100, 100)}, 1200).empty());  // 稳定在内

        // RECONFIG：状态清空 → 目标已在内，重新走滞后确认（born 内但场景空窗
        // ≥1.5s → 豁免；翻转帧 fresh 亦过期）
        re.set_rules(1, {region_in(kBox, "intrusion")});
        CHECK(re.eval(1, {tgt(5, 290, 210, 100, 100)}, 5000).empty());  // pend1
        auto ev = re.eval(1, {tgt(5, 290, 210, 100, 100)}, 6000);       // 翻转 → FIRE
        CHECK(ev.size() == 1 && ev[0].ts_ms == 6000);
    }

    // ---- 7. 真实离开重现：同 id 消失 ≥1.5s 回归 → 重置状态 → 重现进入报警 ----
    // （板上实证修复：报警一次后离开画面再进 ROI 不再报警——tracker 漏检期不
    // 输出目标，inside 停留脏 true，无重现重置则 was_inside=true 永无边沿）
    {
        RuleEngine re;
        re.set_frame_size(1, 640, 480);
        re.set_rules(1, {region_in(kBox, "intrusion")});
        // 走入并触发首次报警
        CHECK(re.eval(1, {tgt(5, 10, 10, 50, 50)}, 0).empty());
        CHECK(re.eval(1, {tgt(5, 280, 200, 100, 100)}, 100).empty());
        auto ev = re.eval(1, {tgt(5, 280, 200, 100, 100)}, 200);
        CHECK(ev.size() == 1 && ev[0].ts_ms == 200);

        // 离开画面（tracker 保留 id，漏检期无输出）：连续空帧
        CHECK(re.eval(1, {}, 1000).empty());
        CHECK(re.eval(1, {}, 2200).empty());

        // 回归画面（同 id）在 ROI 内：gap=2100 ≥1.5s → 重现重置（清 inside/冷却）
        CHECK(re.eval(1, {tgt(5, 280, 200, 100, 100)}, 2300).empty());  // pend1
        ev = re.eval(1, {tgt(5, 280, 200, 100, 100)}, 2400);            // 翻转 → FIRE
        CHECK(ev.size() == 1 && ev[0].ts_ms == 2400);

        // 对照：在内时短暂消失（<1.5s，遮挡级）回归同 id → 无重置，无报警
        CHECK(re.eval(1, {}, 3300).empty());
        CHECK(re.eval(1, {tgt(5, 280, 200, 100, 100)}, 3400).empty());  // gap=1000 无重置
        CHECK(re.eval(1, {tgt(5, 280, 200, 100, 100)}, 3500).empty());
    }

    // ---- 8. 新 id 真实重现：场景空窗 ≥1.5s 后目标直接出现在 ROI 内 → 豁免报警 ----
    // （tracker 删 id 的 >5s 离开走此路径：新 id 繁忙出生抑制不适用——场景空窗
    // 即"真实重现/新进入"证据）
    {
        RuleEngine re;
        re.set_frame_size(1, 640, 480);
        re.set_rules(1, {region_in(kBox, "intrusion")});
        CHECK(re.eval(1, {tgt(11, 280, 200, 100, 100)}, 0).empty());  // born内+空窗 → 豁免
        auto ev = re.eval(1, {tgt(11, 280, 200, 100, 100)}, 100);     // 翻转 → FIRE
        CHECK(ev.size() == 1 && ev[0].ts_ms == 100);
    }

    // ---- 9. 抖动抑制不回退：场景繁忙时新 id 出生在 ROI 内 → 冷静期仍抑制 ----
    // （保住原修复：跟踪 ID 抖动为静止目标反复建 id 的 ~15s 周期误报）
    {
        RuleEngine re;
        re.set_frame_size(1, 640, 480);
        re.set_rules(1, {region_in(kBox, "intrusion")});
        // 暖场目标在外（建立 busy）
        CHECK(re.eval(1, {tgt(20, 10, 10, 50, 50)}, 0).empty());
        // 静止目标 ID 抖动：连续检测中（gap<1.5s）新 id 出生在 ROI 内 → 抑制
        CHECK(re.eval(1, {tgt(21, 280, 200, 100, 100)}, 100).empty());  // born内+busy
        CHECK(re.eval(1, {tgt(21, 280, 200, 100, 100)}, 200).empty());  // 翻转 → fresh 抑制
        CHECK(re.eval(1, {tgt(21, 280, 200, 100, 100)}, 1300).empty()); // 稳定在内
        // 抖动再次换 id：同样抑制（每个新 id 都是繁忙出生）
        CHECK(re.eval(1, {tgt(22, 280, 200, 100, 100)}, 1400).empty());
        CHECK(re.eval(1, {tgt(22, 280, 200, 100, 100)}, 1500).empty());
        CHECK(re.eval(1, {tgt(22, 280, 200, 100, 100)}, 9000).empty()); // 长期无报警
    }

    if (g_fail == 0) printf("rule_engine PASSED\n");
    return g_fail;
}
