#include "rules/RuleEngine.h"

#include <algorithm>
#include <cmath>

#include "common/Log.h"

namespace aibox {

namespace {
// 稳定化滞后帧数：连续 N 帧同向才翻转 inside（3fps 下 ≈0.67s 确认窗）——
// 抗 ROI 边界抖动与检测框交替误判（锚点贴边时原始 in 每帧翻转，直接用会把
// 每次抖动都当"进入"边沿）
constexpr int32_t kStableFrames = 2;
// 新 track 冷静期：首见 1s 内不触发状态规则（状态照常建立）——跟踪器 ID 抖动
// 为同一静止目标建新 id 时，新 id 的"从外到内"是假边沿（板上实证 track 5↔7
// 反复切换导致 region_in 对静止目标 ~15s 误报一次）
constexpr int64_t kTrackGraceMs = 1000;
// 同 (track, 规则) 重复触发冷却：边界振荡目标（锚点贴 ROI 边）即使过了
// 2 帧滞后仍会周期性产生真实边沿（板上实证 ~7-15s 一次）。
// 解除条件：stable 翻转"出"并持续 ≥ kRealLeaveMs（真实离开——边界振荡的
// 短暂出去不满足）→ 冷却解除，下次进入正常触发
constexpr int64_t kTrackRefireMs = 10000;
constexpr int64_t kRealLeaveMs = 2000;
// 真实离开判定窗：同 track 消失 ≥ 该时长后回归（或场景空窗 ≥ 该时长后出现新 id）
// 视为"真实离开后重现"。跟踪器漏检期不输出目标（"检测在哪框哪"），同 id 在此
// 窗口内回归时 inside 停留脏 true、无此重置则离开-回归永不产生进入边沿（板上
// 实证：报警一次后离开画面再进 ROI 不再报警）。取 1.5s：> 2 帧滞后确认窗
// （3fps≈0.67s，短暂遮挡不算离开），< 跟踪器删 id 阈值（missed>15≈5s@3fps，
// 删 id 后走"新 id + 场景空窗豁免"路径，两条路径覆盖无缝衔接）
constexpr int64_t kReappearGapMs = 1500;
}  // namespace

bool point_in_polygon(float px, float py,
                      const std::vector<std::pair<float, float>>& poly, bool* on_edge) {
    if (on_edge) *on_edge = false;
    if (poly.size() < 3) return false;
    const float kEps = 1e-6f;
    bool inside = false;
    const size_t n = poly.size();
    for (size_t i = 0; i < n; ++i) {
        const auto& a = poly[i];
        const auto& b = poly[(i + 1) % n];
        // 边界判定：点在线段上（叉积≈0 且在包围盒内）→ 归属内
        float cross = (b.first - a.first) * (py - a.second) -
                      (b.second - a.second) * (px - a.first);
        if (std::fabs(cross) < kEps && px >= std::min(a.first, b.first) - kEps &&
            px <= std::max(a.first, b.first) + kEps &&
            py >= std::min(a.second, b.second) - kEps &&
            py <= std::max(a.second, b.second) + kEps) {
            if (on_edge) *on_edge = true;
            return true;
        }
        // 射线法（向右水平射线，半开区间处理顶点重复计数）
        if ((a.second > py) != (b.second > py)) {
            float xint = a.first + (py - a.second) * (b.first - a.first) /
                                     (b.second - a.second);
            if (px < xint) inside = !inside;
        }
    }
    return inside;
}

RuleEngine::ChanState& RuleEngine::chan(uint32_t cam) { return chans_[cam]; }

void RuleEngine::set_rules(uint32_t cam, std::vector<Rule> rules) {
    auto& c = chan(cam);
    c.rules = std::move(rules);
    c.tracks.clear();         // set_algos 全量替换语义：track 状态随之作废（track_id 已重置）
    c.track_first_ms.clear();
    c.track_last_ms.clear();
    c.refire.clear();
    // last_targets_ms 保留：RECONFIG 期间站在 ROI 内的目标（通道检测连续）在重建
    // 状态后仍属"繁忙出生"，冷静期抑制——配置动作不得变成误报警
}

void RuleEngine::set_frame_size(uint32_t cam, uint32_t w, uint32_t h) {
    auto& c = chan(cam);
    c.frame_w = w;
    c.frame_h = h;
}

void RuleEngine::reset(uint32_t cam) {
    auto it = chans_.find(cam);
    if (it != chans_.end()) {
        it->second.tracks.clear();
        it->second.track_first_ms.clear();
        it->second.track_last_ms.clear();
        it->second.refire.clear();
    }
}

std::vector<AlarmEvent> RuleEngine::eval(uint32_t cam,
                                         const std::vector<proto::DetTarget>& targets,
                                         int64_t ts_ms) {
    auto& c = chan(cam);
    std::vector<AlarmEvent> out;
    if (c.rules.empty()) return out;

    // 场景繁忙判定（处理前取样）：上次有目标距今 < kReappearGapMs → 出生在 ROI 内
    // 的新 id 大概率是跟踪 ID 抖动（连续检测中的 re-id），走冷静期抑制
    const bool busy = c.last_targets_ms >= 0 &&
                      ts_ms - c.last_targets_ms < kReappearGapMs;

    // 排障探针（事件接线）：每 10 次 eval 打一条（~3s@3fps），观察判定路径内部
    // 状态——锚点坐标 / 内外判定 / track 供给 / 驻留计时。空帧不计频
    bool probe = false;
    if (!targets.empty() && (++c.eval_cnt % 10 == 0)) probe = true;

    for (size_t ti = 0; ti < targets.size(); ++ti) {
        const auto& t = targets[ti];
        // 判定锚点：底边中点（像素 → 归一化）。frame_* 是归一化基准——必须与
        // targets 像素坐标同一坐标系（实际解码帧），postproc 每帧同步
        float ax = (t.x + t.w / 2) / (c.frame_w ? static_cast<float>(c.frame_w) : 1.f);
        float ay = (t.y + t.h) / (c.frame_h ? static_cast<float>(c.frame_h) : 1.f);

        if (t.track_id < 0) {
            // 裸检测（未跟踪）：状态类规则无法判边沿，跳过（防抖层也不适用单点）
            if (probe && ti == 0)
                LOGI("rules", "probe cam=%u f=%ux%u anchor=(%.3f,%.3f) "
                              "track<0 裸检测→状态规则跳过 rules=%zu",
                     cam, c.frame_w, c.frame_h, ax, ay, c.rules.size());
            continue;
        }

        // 真实离开重现检测：同 id 消失 ≥ kReappearGapMs 后回归——重置该 track
        // 全部规则状态与触发冷却。离开画面期间跟踪器保留 id 但不输出目标，
        // inside 停留脏 true，不重置则回归后 was_inside=true 永不产生进入边沿
        auto lit = c.track_last_ms.find(t.track_id);
        if (lit != c.track_last_ms.end() &&
            ts_ms - lit->second >= kReappearGapMs) {
            LOGI("rules", "reappear cam=%u track=%d gap=%lldms → 重置 track 规则状态",
                 cam, t.track_id, static_cast<long long>(ts_ms - lit->second));
            for (auto& rst : c.tracks[t.track_id]) rst = TrackState{};
            for (size_t i = 0; i < c.rules.size(); ++i)
                c.refire.erase(std::make_pair(t.track_id, i));
        }
        c.track_last_ms[t.track_id] = ts_ms;

        for (size_t ri = 0; ri < c.rules.size(); ++ri) {
            const auto& r = c.rules[ri];
            const bool in = point_in_polygon(ax, ay, r.polygon);
            auto& per_rule = c.tracks[t.track_id];
            if (per_rule.size() < c.rules.size()) per_rule.resize(c.rules.size());
            TrackState& st = per_rule[ri];

            // 出生判定（每 track×规则一次）：首见在 ROI 外 = 走入型，进入边沿是
            // 真实走入，豁免冷静期；首见在 ROI 内 = pop-in，仅当场景空窗 ≥
            // kReappearGapMs（真实重现/新进入）才豁免——繁忙场景仍抑制，
            // 保住"静止目标 ID 抖动反复误报"的原修复
            if (!st.born) {
                st.born = true;
                st.grace_exempt = !in || !busy;
            }

            // 新 track 冷静期：首见 1s 内只建状态不触发
            auto fit = c.track_first_ms.find(t.track_id);
            if (fit == c.track_first_ms.end())
                fit = c.track_first_ms.emplace(t.track_id, ts_ms).first;
            const bool fresh = ts_ms - fit->second < kTrackGraceMs;

            // 稳定化内外态：连续 kStableFrames 帧同向才翻转 inside——
            // 单帧抖动（边界贴边/框交替）不产生边沿
            const bool was_inside = st.inside;
            if (in == st.inside) {
                st.pend_in = 0;
                st.pend_out = 0;
            } else if (in) {
                if (++st.pend_in >= kStableFrames) {
                    st.inside = true;
                    st.pend_in = 0;
                    st.pend_out = 0;
                }
            } else {
                if (++st.pend_out >= kStableFrames) {
                    st.inside = false;
                    st.pend_in = 0;
                    st.pend_out = 0;
                }
            }

            bool fire = false;
            switch (r.type) {
                case Type::REGION_IN:
                    fire = st.inside && !was_inside;
                    break;
                case Type::REGION_OUT:
                    fire = !st.inside && was_inside;
                    break;
                case Type::DWELL:
                    if (st.inside) {
                        if (st.dwell_start_ms < 0) st.dwell_start_ms = ts_ms;
                        // armed 在离开时置回；驻留达标且未报过 → 报一次
                        if (st.armed && ts_ms - st.dwell_start_ms >=
                                            static_cast<int64_t>(r.dwell_sec) * 1000) {
                            fire = true;
                            st.armed = false;
                        }
                    } else {
                        st.dwell_start_ms = -1;
                        st.armed = true;  // 离开重新武装
                    }
                    break;
                case Type::STATE_SWITCH:
                    if (st.last_cls >= 0 && st.last_cls != t.cls) fire = true;
                    break;
            }
            if (fresh && !st.grace_exempt)
                fire = false;  // 冷静期：只抑制触发，状态照常建立（出生豁免者除外）

            // 同 (track, 规则) 边沿冷却（真实离开解除）：触发后 kTrackRefireMs
            // 内压制重复边沿；期间 stable 真实离开（out ≥ kRealLeaveMs）解除——
            // 边界振荡（短暂出去又回来）被压制，真实离开再进入正常触发
            if (r.type == Type::REGION_IN || r.type == Type::REGION_OUT) {
                auto fk = std::make_pair(t.track_id, ri);
                ChanState::RefireGuard& g = c.refire[fk];
                // 跟踪 stable 翻转：出（计时）/ 回内（判定解除）
                if (!st.inside) {
                    if (g.out_since_ms < 0) g.out_since_ms = ts_ms;
                } else if (g.out_since_ms >= 0) {
                    if (ts_ms - g.out_since_ms >= kRealLeaveMs) g.last_fire_ms = -1;
                    g.out_since_ms = -1;
                }
                if (fire && g.last_fire_ms >= 0 &&
                    ts_ms - g.last_fire_ms < kTrackRefireMs) {
                    fire = false;
                } else if (fire) {
                    g.last_fire_ms = ts_ms;
                    g.out_since_ms = -1;
                }
            }
            st.last_cls = t.cls;

            if (probe && ti < 3) {  // 打印前 3 个目标（raw=2 时第 2 个才是案发现场）
                LOGI("rules", "probe cam=%u f=%ux%u t[%zu] anchor=(%.3f,%.3f) in=%d "
                              "stable=%d track=%d fresh=%d dwell_start=%lld armed=%d "
                              "ts=%lld rules=%zu",
                     cam, c.frame_w, c.frame_h, ti, ax, ay, static_cast<int>(in),
                     static_cast<int>(st.inside), t.track_id, static_cast<int>(fresh),
                     static_cast<long long>(st.dwell_start_ms),
                     static_cast<int>(st.armed), static_cast<long long>(ts_ms),
                     c.rules.size());
            }

            if (!fire) continue;

            // 防抖合并（架构 §10.4-6）：key=(cam,event_type,cls)，窗口内只报窗口首帧
            auto key = std::make_tuple(cam, r.event_type, t.cls);
            int64_t win_us = static_cast<int64_t>(r.debounce_sec) * 1000000;
            int64_t ts_us = ts_ms * 1000;
            auto it = last_fire_.find(key);
            if (it != last_fire_.end() && ts_us - it->second < win_us) continue;
            last_fire_[key] = ts_us;

            AlarmEvent e;
            e.cam = cam;
            e.event_type = r.event_type;
            e.ts_ms = ts_ms;  // 窗口首帧（合并点）
            e.targets.push_back(t);
            e.algo = r.algo;
            out.push_back(std::move(e));
        }
    }
    if (!targets.empty()) c.last_targets_ms = ts_ms;
    return out;
}

}  // namespace aibox
