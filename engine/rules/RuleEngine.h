// RuleEngine —— 规则判定（任务分解 T2.4，详设 §3.8）
// postproc 线程内、TrackerHub.update 之后串行调用（无锁前提：单通道单线程）
//
// 四类规则（语义定义——详设未逐字给出，此处定稿）：
//   REGION_IN     目标从多边形外 → 内（边沿触发）
//   REGION_OUT    目标从多边形内 → 外（边沿触发）
//   DWELL         目标在多边形内连续驻留 ≥ dwell_sec（只报一次，离开后重新武装）
//   STATE_SWITCH  同一 track 的类别发生变化（如安全帽 on/off），上报新类别事件
//
// 防抖（架构 §10.4-6）：key = (cam, event_type, cls)，debounce_sec 窗口内同类事件合并，
// AlarmEvent.ts 取窗口内首帧时间（快照=首帧，同帧取证语义在 job.cb 闭包实现）
//
// 判定锚点：目标框底边中点（ground 对象贴地进区域的标准做法）；
// 点在多边形边上按"内"处理（边界归属内，确定性三态判定）
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "ipc/Protocol.h"

namespace aibox {

struct AlarmEvent {
    uint32_t cam = 0;
    std::string event_type;
    int64_t ts_ms = 0;  // 窗口首帧时间（防抖合并点，pts 域——内部逻辑用）
    std::vector<proto::DetTarget> targets;
    std::string algo;  // 触发算法（businessd 落库 events.algo_id 用）
};

class RuleEngine {
public:
    enum class Type : uint8_t { REGION_IN, REGION_OUT, DWELL, STATE_SWITCH };

    struct Rule {
        Type type;
        std::vector<std::pair<float, float>> polygon;  // 归一化 0~1（与 set_algos 一致）
        uint32_t dwell_sec = 0;    // DWELL 驻留阈值
        std::string event_type;    // 如 "intrusion" / "no_helmet"
        uint32_t debounce_sec = 0; // 事件防抖窗
        std::string algo;          // 触发算法 id（事件溯源）
    };

    // RECONFIG 全量替换（set_algos 语义）；同时清空 track 状态（track_id 已重置）
    void set_rules(uint32_t cam, std::vector<Rule> rules);

    // 输入：跟踪后的 targets（原始分辨率坐标 + track_id）；返回本轮新产出的事件
    // 注意：targets 坐标是原始像素，polygon 是归一化——eval 内部按帧尺寸换算，
    // 帧宽高由 set_frame_size 提供（RECONFIG 时随分辨率变更）
    void set_frame_size(uint32_t cam, uint32_t w, uint32_t h);
    std::vector<AlarmEvent> eval(uint32_t cam, const std::vector<proto::DetTarget>& targets,
                                 int64_t ts_ms);
    void reset(uint32_t cam);  // 断流/RECONFIG 清空（last_fire 保留防抖语义，可一并清）

private:
    struct TrackState {
        bool inside = false;     // 稳定化内外态（连续 2 帧同向才翻转，非原始单帧判定）
        bool armed = true;       // DWELL：离开区域后重新武装
        int64_t dwell_start_ms = -1;
        int32_t last_cls = -1;   // STATE_SWITCH
        int32_t pend_in = 0;     // 连续"原始在内"帧数（滞后切换计数）
        int32_t pend_out = 0;    // 连续"原始在外"帧数
        bool born = false;        // 已做出生判定（每 track×规则一次性，首帧 eval 时置位）
        bool grace_exempt = false;  // 出生豁免：true = 不受新 track 冷静期抑制
    };
    struct ChanState {
        std::vector<Rule> rules;
        uint32_t frame_w = 0, frame_h = 0;
        uint32_t eval_cnt = 0;  // 排障探针计数（每 N 次 eval 打一条状态）
        // key = track_id（<0 的裸检测跳过状态规则）；
        // value 按"规则下标"隔离——inside/dwell/last_cls 都是单条规则的语义，
        // 多条规则（如 REGION_IN + REGION_OUT 共用同一多边形）若共享一份状态，
        // 先评估的规则会改写 inside，后评估的永远看不到边沿（板上实测踩中）
        std::map<int32_t, std::vector<TrackState>> tracks;
        // track 首见 ts_ms（冷静期：新 track 短窗内不触发状态规则——跟踪器
        // ID 抖动会为同一静止目标反复建新 id，每个新 id 在 ROI 内都是假"进入"）
        std::map<int32_t, int64_t> track_first_ms;
        // (track_id, 规则下标) → 边沿冷却态 {上次触发 ts, 翻转出内外的 ts}：
        // 触发后冷却 kTrackRefireMs；期间 stable 真实离开（out 持续 ≥
        // kRealLeaveMs）即解除冷却——真实离开再进入正常触发，边界振荡
        // （短暂出去又回来）仍被压制
        struct RefireGuard {
            int64_t last_fire_ms = -1;
            int64_t out_since_ms = -1;  // stable=0 的起始时刻（-1=在内）
        };
        std::map<std::pair<int32_t, size_t>, RefireGuard> refire;
        // track 末次出现时刻（真实离开检测）：跟踪器漏检期不输出目标（"检测在哪
        // 框哪"），同 id 消失 ≥ kReappearGapMs 后回归 = 真实离开后重现 → 重置该
        // track 全部规则状态——否则 inside 停留脏 true，离开-回归永不产生进入边沿
        //（板上实证：报警一次后离开画面再进 ROI 不再报警）
        std::map<int32_t, int64_t> track_last_ms;
        // 通道级末次有目标时刻（新 id 出生豁免判定的"场景繁忙"基准）：
        // 连续检测中（gap < kReappearGapMs）出生在 ROI 内的新 id 大概率是跟踪
        // ID 抖动 → 冷静期抑制（保住"静止目标反复误报"的原修复）；场景空窗
        // ≥ kReappearGapMs 后出现的目标 = 真实重现/新进入 → 豁免。
        // set_rules 不清此字段：RECONFIG 期间站在 ROI 内的目标（通道检测连续）
        // 重建状态后仍属"繁忙出生"，冷静期抑制——配置动作不得变成误报警
        int64_t last_targets_ms = -1;
    };

    ChanState& chan(uint32_t cam);

    std::map<uint32_t, ChanState> chans_;
    // 防抖：key = (cam, event_type, cls) → 窗口首帧 ts
    std::map<std::tuple<uint32_t, std::string, int32_t>, int64_t> last_fire_;
};

// 点在多边形内（射线法 + 边界归属内）；on_edge 参数返回"点在边上"
bool point_in_polygon(float px, float py, const std::vector<std::pair<float, float>>& poly,
                      bool* on_edge = nullptr);

}  // namespace aibox
