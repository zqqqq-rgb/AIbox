// TrackerHub —— 任务分解 T2.3，详设 §3.7
// 每通道一个 ByteTrack 风格跟踪器：两阶段关联（高分/低分）+ Kalman 常速外推
// postproc 线程池内串行执行（per channel），纯逻辑：无线程/无锁/无硬件依赖
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "ipc/Protocol.h"

namespace aibox {
namespace track {

// 单通道 ByteTrack 风格跟踪器
class ByteTrack {
public:
    // 就地填充 dets 中每个元素的 track_id / vx / vy（px/s）；
    // 本帧未匹配但存活的 track 只计入 missed、不输出（板上裁决：预测框不是
    // 检测，输出会造成旧框残留——"检测在哪框哪"）；恢复检测后 id 不变
    void update(std::vector<proto::DetTarget>& dets, int64_t ts_ms);
    void reset();  // RECONFIG/断流清空（track_id 重新从 1 计）

private:
    // 8 维联合 CV Kalman 的解耦近似：对 (cx,cy,w,h) 各自独立的一维 [位置,速度] 滤波
    //（位置-速度互协方差弱、每帧个位数目标的稀疏场景精度足够，免掉矩阵库依赖）
    struct Kalman1D {
        Kalman1D() = default;
        explicit Kalman1D(float pos) : p(pos) {}

        void predict(float dt);  // dt 单位秒；速度状态单位 px/s
        void update(float z);    // 只观测位置（H=[1,0]）

        float p = 0, v = 0;  // 状态：位置 / 速度（px/s）
        // 初始协方差：位置与速度均按强不确定处理，前几帧靠增益快速收敛
        float c00 = 100.f, c01 = 0.f, c10 = 0.f, c11 = 100.f;
    };

    struct Track {
        int32_t id = 0;
        Kalman1D kx, ky, kw, kh;  // cx/cy/w/h 四路独立 CV 滤波（KF 状态存中心点）
        int32_t cls = 0;
        float conf = 0;  // 最近观测的 cls/conf（predicted() 关联比对/历史输出沿用）
        int missed = 0;  // 连续未匹配帧数（>15 删）

        // Kalman 预测框（IoU 关联比对用）；x,y 为左上角（types.h 契约）
        // 注：漏检期不再把 predicted() 追加到输出（板上裁决"检测在哪框哪"）
        proto::DetTarget predicted() const {
            proto::DetTarget b;
            b.cls = cls;
            b.conf = conf;
            b.x = kx.p - kw.p * 0.5f;
            b.y = ky.p - kh.p * 0.5f;
            b.w = kw.p > 1.f ? kw.p : 1.f;  // 长时间外推防宽高退化为负（破坏 IoU 语义）
            b.h = kh.p > 1.f ? kh.p : 1.f;
            b.track_id = id;
            return b;
        }
    };

    std::vector<Track> tracks_;
    int32_t next_id_ = 1;
    int64_t last_ts_ms_ = 0;
    bool has_ts_ = false;  // 首帧标记：首帧 vx/vy=0，不做断流判断
};

// 每通道一个 ByteTrack 的管理器（postproc 线程内串行调用，无需加锁）
class TrackerHub {
public:
    void update(uint32_t cam, std::vector<proto::DetTarget>& dets, int64_t ts_ms);
    void reset(uint32_t cam);

private:
    std::map<uint32_t, ByteTrack> channels_;
};

}  // namespace track
}  // namespace aibox
