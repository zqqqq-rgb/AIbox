// T4.6 单测（§4-T4.6 断言表）：
//   ① 伪文件系统 95% 水位：usage_fn 注入 → 最旧 segments 文件+行被删，最新保留
//   ② 保留天数：>keep_days 删、新行留
//   ③ batch_max 单轮上限
//   ④ Outbox 断网缓存重传：假发布器 ack → outbox 删行 + events.reported 对账置位
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "janitor.h"
#include "test_util.h"

using namespace aibox;
using namespace aibox::biz;

namespace {

// 失败即返回（依赖前置成立的断言必须用 REQUIRE——板上实测：CHECK 失败后继续
// 跑 + 空容器解引用 = SegFault 掩盖真实错误）
#define REQUIRE(cond)                                                        \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "REQUIRE FAIL %s:%d: %s\n", __FILE__, __LINE__,  \
                    #cond);                                                  \
            g_fail = 1;                                                      \
            return;                                                          \
        }                                                                    \
    } while (0)

#define REQUIRE_MSG(cond, ...)                                               \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "REQUIRE FAIL %s:%d: %s  ", __FILE__, __LINE__,  \
                    #cond);                                                  \
            fprintf(stderr, __VA_ARGS__);                                    \
            fprintf(stderr, "\n");                                           \
            g_fail = 1;                                                      \
            return;                                                          \
        }                                                                    \
    } while (0)

// 目录内普通文件计数（注入 usage 用：每文件代表固定水位增量）
int count_files(const std::string& dir_path) {
    DIR* d = ::opendir(dir_path.c_str());
    if (!d) return 0;
    int n = 0;
    while (dirent* e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        ++n;
    }
    ::closedir(d);
    return n;
}

// 临时目录脚手架（真实小文件验证 unlink）
struct TempDir {
    std::string path;
    TempDir() {
        path = "/tmp/aibox_t_janitor_" + std::to_string(::getpid());
        ::mkdir(path.c_str(), 0755);
    }
    ~TempDir() { ::rmdir(path.c_str()); }
    std::string file(const char* name) { return path + "/" + name; }
};

bool file_exists(const std::string& p) { return ::access(p.c_str(), F_OK) == 0; }

// ① 95% 水位删最旧（§4-T4.6 断言）
void test_watermark_sweep() {
    TempDir dir;
    Db db(":memory:");
    CHECK(db.migrate() == Db::kSchemaLatest);

    const int64_t base = 1700000000000LL;  // ms
    std::vector<int64_t> ids;
    for (int i = 0; i < 3; ++i) {
        SegmentRow s;
        s.camera_id = 1;
        s.file_path = dir.file(("seg" + std::to_string(i) + ".mp4").c_str());
        s.start_ts = base + i * 300000;  // 0 最旧
        CHECK(db.insert_segment(s) > 0);
        ids.push_back(s.start_ts);
        // 建真实文件（unlink 验证）
        FILE* f = std::fopen(s.file_path.c_str(), "w");
        CHECK(f != nullptr);
        if (f) std::fclose(f);
    }
    // 注入：水位按目录内真实文件数计（每文件 5%，基线 0.80）——
    // janitor 在删除循环内重算 usage_fn，注入函数必须反映"已删"状态；
    // 闭包变量方案（0.95-deleted*0.05）在 sweep 内恒 0.95 → 删光全表（板上实测踩中）
    auto usage = [&dir](const std::string&) {
        return 0.80 + count_files(dir.path) * 0.05;  // 3 文件=0.95 / 2=0.90 / 1=0.85
    };

    DiskJanitor::Params p;
    p.mp4_dir = dir.path;
    p.keep_days = 0;  // 只测水位
    DiskJanitor jan(db, p, usage);
    const int n = jan.sweep_once();
    REQUIRE_MSG(n == 1, "n=%d（0.95 → 删 1 → 0.90 ≤ 水位停）", n);
    // 断言：最旧（start_ts=base）文件+行被删，最新保留
    REQUIRE(!file_exists(dir.file("seg0.mp4")));
    REQUIRE(file_exists(dir.file("seg1.mp4")));
    REQUIRE(file_exists(dir.file("seg2.mp4")));
    const auto rows = db.list_segments(1, 0, 0, 10);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].start_ts == base + 600000);          // DESC：最新在前
    CHECK(rows.back().start_ts == base + 300000);      // 剩余最旧 = 原 1 号
}

// ② 保留天数
void test_keep_days() {
    TempDir dir;
    Db db(":memory:");
    CHECK(db.migrate() == Db::kSchemaLatest);
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    SegmentRow old_s;
    old_s.camera_id = 1;
    old_s.file_path = dir.file("old.mp4");
    old_s.start_ts = now - 40LL * 86400000;  // 40 天前
    CHECK(db.insert_segment(old_s) > 0);
    FILE* f = std::fopen(old_s.file_path.c_str(), "w");
    if (f) std::fclose(f);
    SegmentRow fresh;
    fresh.camera_id = 1;
    fresh.file_path = dir.file("fresh.mp4");
    fresh.start_ts = now - 10LL * 86400000;  // 10 天前
    CHECK(db.insert_segment(fresh) > 0);
    FILE* ff = std::fopen(fresh.file_path.c_str(), "w");  // fresh 也要真实文件
    REQUIRE_MSG(ff != nullptr, "fopen fresh.mp4 fail");
    if (ff) std::fclose(ff);

    DiskJanitor::Params p;
    p.mp4_dir = dir.path;
    p.keep_days = 30;
    DiskJanitor jan(db, p, [](const std::string&) { return 0.1; });  // 低水位
    const int n = jan.sweep_once();
    CHECK(n == 1);
    CHECK(!file_exists(dir.file("old.mp4")));
    CHECK(file_exists(dir.file("fresh.mp4")));
    CHECK(db.list_segments(1, 0, 0, 10).size() == 1);
}

// ③ batch_max 上限
void test_batch_max() {
    TempDir dir;
    Db db(":memory:");
    CHECK(db.migrate() == Db::kSchemaLatest);
    const int64_t base = 1700000000000LL;
    for (int i = 0; i < 5; ++i) {
        SegmentRow s;
        s.camera_id = 1;
        s.file_path = dir.file(("b" + std::to_string(i) + ".mp4").c_str());
        s.start_ts = base + i * 1000;
        CHECK(db.insert_segment(s) > 0);
    }
    DiskJanitor::Params p;
    p.mp4_dir = dir.path;
    p.keep_days = 0;
    p.batch_max = 2;
    DiskJanitor jan(db, p, [](const std::string&) { return 0.95; });  // 恒超限
    const int n = jan.sweep_once();
    CHECK(n == 2);  // 单轮只删 batch_max 个（不因恒超限死循环）
    CHECK(db.list_segments(1, 0, 0, 10).size() == 3);
}

// ④ Outbox 重传 + events.reported 对账
struct FakePublisher : IMqttPublisher {
    bool ok = true;
    int published = 0;
    std::vector<std::pair<std::string, std::string>> sent;
    bool publish(const std::string& topic, const std::string& payload) override {
        ++published;
        if (ok) sent.emplace_back(topic, payload);
        return ok;
    }
};

void test_outbox_retransmit() {
    Db db(":memory:");
    CHECK(db.migrate() == Db::kSchemaLatest);
    // 事件 + 入 outbox（main 装配语义：payload 带 _event_id）
    for (int64_t eid : {1, 2, 3}) {
        EventRow e;
        e.camera_id = 1;
        e.algo_id = "person";
        e.type = "REGION_IN";
        e.ts = eid;
        e.targets_json = "[]";
        CHECK(db.insert_event(e) == eid);  // 自增从 1 起
        db.insert_outbox("aibox/events/1", "{\"_event_id\":" + std::to_string(eid) +
                                               ",\"type\":\"REGION_IN\"}");
    }

    FakePublisher pub;
    OutboxRetransmitter tx(db, &pub);

    // 断网：publish 首条即失败 → break 保序，0 条确认、全保留（1 次尝试）
    pub.ok = false;
    REQUIRE_MSG(tx.drain_once() == 0, "断网轮应 0 确认");
    REQUIRE(db.pending_outbox(10).size() == 3);

    // 联网：全部确认 → outbox 清空 + reported 置位
    // published 累计 = 断网轮 1 次失败尝试 + 联网轮 3 次成功 = 4
    pub.ok = true;
    REQUIRE_MSG(tx.drain_once() == 3, "联网轮应确认 3 条");
    REQUIRE(db.pending_outbox(10).empty());
    CHECK(pub.published == 4);
    const auto evs = db.list_events(1, "", 0, 0, 10);
    REQUIRE(evs.size() == 3);
    for (const auto& e : evs) CHECK_MSG(e.reported, "event %lld 应已对账",
                                        static_cast<long long>(e.id));
}

void run() {
    // 构建标记：缺此行 = 板上跑的是陈旧二进制（勿信失败行号）
    fprintf(stderr, "[diag] test_janitor v3 (fresh-file + drain-no-running + count=4)\n");
    test_watermark_sweep();
    test_keep_days();
    test_batch_max();
    test_outbox_retransmit();
}

}  // namespace

TEST_MAIN("janitor")
