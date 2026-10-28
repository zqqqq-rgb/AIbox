#include "track/TrackerHub.h"

#include <algorithm>

namespace aibox {
namespace track {
namespace {

// ---- 调参（一维解耦滤波的对角固定值）----
constexpr float kR = 1.0f;            // 测量噪声方差(px^2)：检测框抖动
constexpr float kQp = 1.0f;           // 位置过程噪声方差：允许预测滞后被快速拉回
constexpr float kQv = 25.0f;          // 速度过程噪声方差((px/s)^2)：容忍加减速渐变
constexpr float kConfHigh = 0.5f;     // 高/低分检测分界（ByteTrack 第一阈值）
constexpr float kConfLow = 0.1f;      // 低于此值的检测不参与关联（近噪声级误检）
constexpr float kIouThres = 0.3f;     // 关联 IoU 阈值
constexpr int kMaxMissed = 15;        // 连续未匹配上限（≈5s@3fps），超出删 track
constexpr int64_t kGapResetMs = 2000; // 帧间隔超过 2s 视为断流

// 左上角坐标系 IoU（x,y,w,h 契约见 interfaces/types.h）
float iou(const proto::DetTarget& a, const proto::DetTarget& b) {
    const float ix = std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x);
    const float iy = std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y);
    if (ix <= 0.f || iy <= 0.f) return 0.f;
    const float inter = ix * iy;
    const float uni = a.w * a.h + b.w * b.h - inter;
    return uni > 0.f ? inter / uni : 0.f;
}

// 关联候选对（贪心排序用）
struct Pair {
    float score;
    int det;
    int trk;
};

}  // namespace

// ---- Kalman1D：一维常速（CV）滤波，手写标量公式 ----

void ByteTrack::Kalman1D::predict(float dt) {
    p += v * dt;
    // P = F·P·Fᵀ + Q，F=[1,dt;0,1]，Q=diag(kQp,kQv)：展开成四个标量避免矩阵记号
    const float n00 = c00 + dt * (c10 + c01) + dt * dt * c11 + kQp;
    const float n01 = c01 + dt * c11;
    const float n10 = c10 + dt * c11;
    const float n11 = c11 + kQv;
    c00 = n00;
    c01 = n01;
    c10 = n10;
    c11 = n11;
}

void ByteTrack::Kalman1D::update(float z) {
    // H=[1,0]（只观测位置），标准标量更新；S=c00+kR
    const float s = c00 + kR;
    if (s <= 0.f) return;  // 数值防护
    const float k0 = c00 / s;
    const float k1 = c10 / s;
    const float innov = z - p;
    p += k0 * innov;
    v += k1 * innov;
    // P = (I-KH)·P，用旧值算完后一次性写回
    const float n00 = (1.f - k0) * c00;
    const float n01 = (1.f - k0) * c01;
    const float n10 = c10 - k1 * c00;
    const float n11 = c11 - k1 * c01;
    c00 = n00;
    c01 = n01;
    c10 = n10;
    c11 = n11;
}

// ---- ByteTrack ----

void ByteTrack::update(std::vector<proto::DetTarget>& dets, int64_t ts_ms) {
    // 断流防护：时间倒退或间隔 >2s 清空重来，防 Kalman 速度外推爆炸
    if (has_ts_ && (ts_ms < last_ts_ms_ || ts_ms - last_ts_ms_ > kGapResetMs)) {
        reset();
    }
    const bool first = !has_ts_;
    float dt = 0.f;  // 秒；Kalman 速度状态单位 px/s
    if (!first && ts_ms > last_ts_ms_) {
        dt = static_cast<float>(ts_ms - last_ts_ms_) / 1000.f;
    }
    last_ts_ms_ = ts_ms;
    has_ts_ = true;
    const bool vel_ok = !first && dt > 0.f;  // 契约：首帧/dt≤0 输出 vx=vy=0

    // 全部存活 track 先预测到当前帧（关联基于预测框；漏检帧即纯外推）
    for (Track& t : tracks_) {
        t.kx.predict(dt);
        t.ky.predict(dt);
        t.kw.predict(dt);
        t.kh.predict(dt);
    }

    // 按置信度分桶：高分/低分走两阶段关联，低于 kConfLow 的不参与（只标注不剔除）
    std::vector<int> high, low;
    for (size_t i = 0; i < dets.size(); ++i) {
        if (dets[i].conf >= kConfHigh) {
            high.push_back(static_cast<int>(i));
        } else if (dets[i].conf >= kConfLow) {
            low.push_back(static_cast<int>(i));
        }
    }

    const int n_old = static_cast<int>(tracks_.size());
    std::vector<int> det_trk(static_cast<int>(dets.size()), -1);  // det → track 下标
    std::vector<int> trk_det(n_old, -1);                          // track → det 下标

    // 匈牙利算法的贪心近似：IoU 降序逐一配对、跳过已占用的检测/轨迹。
    // 检测稀疏（每帧个位数目标）场景与全局最优等价，省去 O(n^3) 指派求解
    auto greedy = [&](const std::vector<int>& cand) {
        if (cand.empty() || n_old == 0) return;
        std::vector<Pair> pairs;
        for (int di : cand) {
            for (int ti = 0; ti < n_old; ++ti) {
                if (trk_det[ti] >= 0) continue;
                const float s = iou(dets[di], tracks_[ti].predicted());
                if (s >= kIouThres) pairs.push_back({s, di, ti});
            }
        }
        // 平手按 (检测序号, 轨迹序号) 决出全序：排序结果跨平台确定
        std::sort(pairs.begin(), pairs.end(), [](const Pair& l, const Pair& r) {
            if (l.score != r.score) return l.score > r.score;
            if (l.det != r.det) return l.det < r.det;
            return l.trk < r.trk;
        });
        for (const Pair& pr : pairs) {
            if (det_trk[pr.det] >= 0 || trk_det[pr.trk] >= 0) continue;
            det_trk[pr.det] = pr.trk;
            trk_det[pr.trk] = pr.det;
        }
    };
    greedy(high);  // 阶段1：高分检测 × 全部存活 track
    greedy(low);   // 阶段2：低分检测 × 第一轮未匹配 track（ByteTrack 低分回收）

    // 已匹配：KF 观测修正 + 就地填充 track_id / vx / vy（框坐标不动）
    for (size_t di = 0; di < dets.size(); ++di) {
        const int ti = det_trk[di];
        if (ti < 0) continue;
        Track& t = tracks_[ti];
        const proto::DetTarget& d = dets[di];
        t.kx.update(d.x + d.w * 0.5f);
        t.ky.update(d.y + d.h * 0.5f);
        t.kw.update(d.w);
        t.kh.update(d.h);
        t.cls = d.cls;
        t.conf = d.conf;
        t.missed = 0;
        dets[di].track_id = t.id;
        dets[di].vx = vel_ok ? t.kx.v : 0.f;
        dets[di].vy = vel_ok ? t.ky.v : 0.f;
    }

    // 未匹配检测：显式标注 -1/0（低分不建新 track，防误检生根）
    for (size_t di = 0; di < dets.size(); ++di) {
        if (det_trk[di] >= 0) continue;
        dets[di].track_id = -1;
        dets[di].vx = 0.f;
        dets[di].vy = 0.f;
    }

    // 高分未匹配检测 → 新 track（id 从 1 递增；新 track 首帧速度为 0）
    std::vector<Track> fresh;
    for (int di : high) {
        if (det_trk[di] >= 0) continue;
        const proto::DetTarget& d = dets[di];
        Track t;
        t.id = next_id_++;
        t.kx = Kalman1D(d.x + d.w * 0.5f);
        t.ky = Kalman1D(d.y + d.h * 0.5f);
        t.kw = Kalman1D(d.w);
        t.kh = Kalman1D(d.h);
        t.cls = d.cls;
        t.conf = d.conf;
        dets[di].track_id = t.id;
        fresh.push_back(t);
    }

    // 未匹配老 track：漏检计数；missed ≤ kMaxMissed 期间 track 仍存活
    // （恢复检测后 id 不变——RuleEngine 状态/事件去重连续），但**不输出预测框**：
    // 板上裁决"检测在哪框哪"，KF 预测位置不是检测——输出会让目标移走后旧框
    // 残留最长 5s（15 帧@3fps），与新位置检测框并存（板上 static 场景实证）
    std::vector<Track> alive;
    alive.reserve(static_cast<size_t>(n_old) + fresh.size());
    for (int ti = 0; ti < n_old; ++ti) {
        Track& t = tracks_[ti];
        if (trk_det[ti] >= 0) {
            alive.push_back(t);
            continue;
        }
        ++t.missed;
        if (t.missed > kMaxMissed) continue;  // 删除且不再输出
        alive.push_back(t);                   // 只存活，不输出
    }
    alive.insert(alive.end(), fresh.begin(), fresh.end());
    tracks_.swap(alive);
}

void ByteTrack::reset() {
    tracks_.clear();
    next_id_ = 1;
    last_ts_ms_ = 0;
    has_ts_ = false;
}

// ---- TrackerHub ----

void TrackerHub::update(uint32_t cam, std::vector<proto::DetTarget>& dets, int64_t ts_ms) {
    channels_[cam].update(dets, ts_ms);  // 首见通道自动建 track
}

void TrackerHub::reset(uint32_t cam) {
    const auto it = channels_.find(cam);
    if (it != channels_.end()) it->second.reset();
}

}  // namespace track
}  // namespace aibox
