// T2.3 单测（任务分解 §4-T2.3，确定性无随机）
// 1) track_id 连续性：30px/s 匀速目标，3fps×180 帧 —— 全程同一 id 无跳变
// 2) 漏检保持：中段挖 10 帧 —— 漏检期无输出，恢复后 id 不变
// 3) vx 方向与量级：稳定后 |vx-30|/30<0.3，vy≈0，vx>0
// 4) 两目标不串：30/80px/s 同向 —— 180 帧后仍两个不同 id
// 5) 低分回收：conf=0.3 帧仍关联既有 track（不新增 id）
// 6) reset 后 id 重开（ByteTrack 与 TrackerHub 双层验证）
#include "track/TrackerHub.h"
#include "test_util.h"

#include <cmath>
#include <vector>

using namespace aibox;

namespace {

constexpr int64_t kDtMs = 333;  // 3fps（默认检测帧率，抽帧后目标帧间位移显著）

proto::DetTarget make_det(float x, float y, float w, float h, float conf, int32_t cls = 0) {
    proto::DetTarget d;
    d.cls = cls;
    d.conf = conf;
    d.x = x;
    d.y = y;
    d.w = w;
    d.h = h;
    return d;
}

void run() {
    // --- 1) + 3) 单目标 30px/s 匀速右移，180 帧 ---
    {
        track::ByteTrack trk;
        const float x0 = 100.f, y0 = 200.f, size = 50.f, speed = 30.f;
        int first_id = -1;
        bool one_box = true;   // 每帧输出恰好 1 框（无鬼影泄漏）
        bool same_id = true;
        float last_vx = 0.f, last_vy = 0.f;
        for (int i = 0; i < 180; ++i) {
            const int64_t ts = i * kDtMs;
            std::vector<proto::DetTarget> dets;
            dets.push_back(make_det(x0 + speed * (ts / 1000.f), y0, size, size, 0.9f));
            trk.update(dets, ts);
            if (dets.size() != 1) one_box = false;
            if (dets[0].track_id <= 0) same_id = false;
            if (first_id < 0) first_id = dets[0].track_id;
            if (dets[0].track_id != first_id) same_id = false;
            last_vx = dets[0].vx;
            last_vy = dets[0].vy;
        }
        CHECK(one_box);
        CHECK(same_id);
        CHECK(first_id == 1);  // id 从 1 递增
        // 3) 稳定后的速度：方向向右、量级 ±30% 内、纵向无漂移
        CHECK(last_vx > 0.f);
        CHECK(std::fabs(last_vx - speed) / speed < 0.3f);
        CHECK(std::fabs(last_vy) < 0.5f);
    }

    // --- 2) 漏检保持：中段连续挖 10 帧 ---
    {
        track::ByteTrack trk;
        const float x0 = 100.f, y0 = 200.f, size = 50.f, speed = 30.f;
        int id = -1;
        for (int i = 0; i < 40; ++i) {
            const int64_t ts = i * kDtMs;
            std::vector<proto::DetTarget> dets;
            dets.push_back(make_det(x0 + speed * (ts / 1000.f), y0, size, size, 0.9f));
            trk.update(dets, ts);
            id = dets[0].track_id;
        }
        CHECK(id == 1);
        // 挖 10 帧：传空数组但时间前进 → 不输出预测框（板上裁决：预测框不是检测），
        // 但 track 存活——恢复检测后 id 不变
        bool gap_ok = true;
        for (int i = 40; i < 50; ++i) {
            const int64_t ts = i * kDtMs;
            std::vector<proto::DetTarget> dets;  // 空：漏检
            trk.update(dets, ts);
            if (!dets.empty()) gap_ok = false;   // 漏检期无任何输出
        }
        CHECK(gap_ok);
        // 恢复检测：id 不变
        const int64_t ts_back = 50 * kDtMs;
        std::vector<proto::DetTarget> dets;
        dets.push_back(make_det(x0 + speed * (ts_back / 1000.f), y0, size, size, 0.9f));
        trk.update(dets, ts_back);
        CHECK(dets.size() == 1);
        CHECK(dets[0].track_id == id);
        // 再跟 10 帧确认稳定延续
        bool stable = true;
        for (int i = 51; i < 60; ++i) {
            const int64_t ts = i * kDtMs;
            std::vector<proto::DetTarget> dd;
            dd.push_back(make_det(x0 + speed * (ts / 1000.f), y0, size, size, 0.9f));
            trk.update(dd, ts);
            if (dd.size() != 1 || dd[0].track_id != id) stable = false;
        }
        CHECK(stable);
    }

    // --- 4) 两目标不串：30/80px/s 同向不同速 ---
    {
        track::ByteTrack trk;
        // 80px 框：80px/s 目标帧间位移 26.6px，纯 IoU≈0.50 仍高于阈值 0.3，可被关联
        const float size = 80.f;
        int slow_id = -1, fast_id = -1;
        int slow_first = -1, fast_first = -1;
        bool two_boxes = true;
        for (int i = 0; i < 180; ++i) {
            const int64_t ts = i * kDtMs;
            const float t = ts / 1000.f;
            std::vector<proto::DetTarget> dets;
            dets.push_back(make_det(100.f + 30.f * t, 100.f, size, size, 0.9f, 0));
            dets.push_back(make_det(600.f + 80.f * t, 300.f, size, size, 0.9f, 1));
            trk.update(dets, ts);
            if (dets.size() != 2) two_boxes = false;  // 无鬼影 → 两目标全程被关联
            for (const proto::DetTarget& d : dets) {
                if (d.cls == 0) slow_id = d.track_id;
                if (d.cls == 1) fast_id = d.track_id;
            }
            if (i == 10) {
                slow_first = slow_id;
                fast_first = fast_id;
            }
        }
        CHECK(two_boxes);
        CHECK(slow_id > 0 && fast_id > 0 && slow_id != fast_id);
        CHECK(slow_id == slow_first && fast_id == fast_first);  // 无 id 漂移/串道
    }

    // --- 5) 低分回收：conf=0.3 仍关联既有 track ---
    {
        track::ByteTrack trk;
        const float x0 = 100.f, y0 = 200.f, size = 50.f, speed = 30.f;
        int id = -1;
        for (int i = 0; i < 10; ++i) {
            const int64_t ts = i * kDtMs;
            std::vector<proto::DetTarget> dets;
            dets.push_back(make_det(x0 + speed * (ts / 1000.f), y0, size, size, 0.9f));
            trk.update(dets, ts);
            id = dets[0].track_id;
        }
        CHECK(id == 1);
        // 低分帧：被阶段2回收关联既有 track（若未回收则该检测 id=-1，无预测框输出）
        const int64_t ts_low = 10 * kDtMs;
        std::vector<proto::DetTarget> dets;
        dets.push_back(make_det(x0 + speed * (ts_low / 1000.f), y0, size, size, 0.3f));
        trk.update(dets, ts_low);
        CHECK(dets.size() == 1);
        CHECK(dets[0].track_id == id);  // 不新增 id
        // 恢复高分后 id 延续
        const int64_t ts2 = 11 * kDtMs;
        std::vector<proto::DetTarget> dets2;
        dets2.push_back(make_det(x0 + speed * (ts2 / 1000.f), y0, size, size, 0.9f));
        trk.update(dets2, ts2);
        CHECK(dets2.size() == 1);
        CHECK(dets2[0].track_id == id);
    }

    // --- 6) reset 后 id 重开 ---
    {
        track::ByteTrack trk;
        std::vector<proto::DetTarget> dets;
        dets.push_back(make_det(10.f, 10.f, 50.f, 50.f, 0.9f));
        trk.update(dets, 0);
        CHECK(dets[0].track_id == 1);
        trk.reset();  // RECONFIG/断流清空
        std::vector<proto::DetTarget> dets2;
        dets2.push_back(make_det(10.f, 10.f, 50.f, 50.f, 0.9f));
        trk.update(dets2, kDtMs);
        CHECK(dets2[0].track_id == 1);  // 重新从 1 计
    }
    // TrackerHub：通道 id 独立，reset 单通道不波及其他通道
    {
        track::TrackerHub hub;
        std::vector<proto::DetTarget> a, b;
        a.push_back(make_det(10.f, 10.f, 50.f, 50.f, 0.9f));
        b.push_back(make_det(10.f, 10.f, 50.f, 50.f, 0.9f));
        hub.update(0, a, 0);
        hub.update(1, b, 0);
        CHECK(a[0].track_id == 1 && b[0].track_id == 1);  // 各通道独立从 1 计
        hub.reset(0);
        std::vector<proto::DetTarget> a2, b2;
        a2.push_back(make_det(10.f, 10.f, 50.f, 50.f, 0.9f));
        b2.push_back(make_det(10.f, 10.f, 50.f, 50.f, 0.9f));
        hub.update(0, a2, kDtMs);
        hub.update(1, b2, kDtMs);
        CHECK(a2[0].track_id == 1);  // cam0 重开后 id 从 1
        CHECK(b2[0].track_id == 1);  // cam1 未 reset，id 延续
    }
}

}  // namespace

TEST_MAIN("test_tracker")
