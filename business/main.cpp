// businessd 业务进程入口（任务分解 T4.x 装配，详设 §5.7 启动序列）
//
// 启动序（详设 §5.7）：
//   1. 读配置（JSON，--config 指定；默认 ./businessd.conf）
//   2. Db open + migration（版本表幂等）
//   3. HookServer :8090 起（先于 ZLM 拉起也无妨，hook 失败 ZLM 会重试）
//   4. HttpApi :80 起（静态 web/ 托管 + MP4 /recordings）
//   5. WsServer :8081 起（REST 独立端口；浏览器 WS 不受 CORS 限制）
//   6. UDS connect core-engine → SYNCING：
//      ensure_proxies = getMediaList 对比 DB，ZLM 重启过的代理逐台重建（幂等）
//      → 逐台 add_cam/set_algos（UdsClient 内部实现）→ ONLINE
//   7. Outbox 重传器（MQTT V1 概要：接口就位，云对接属后续里程碑，默认关闭）
//   8. ready=true（/api/system/info ready 字段）
//
// 事件分发（UdsClient on_evt，详设 §5.2 ONLINE 表）：
//   detections     → WsServer /ws/meta/{cam} 组播（原始行透传）
//   event          → DB 插入 + WS 广播 + outbox（MQTT 断网缓存）
//   camera_status  → DB online 字段 + WS 广播
//   degrade        → WS 广播 + LOGI
//   registry_ready → algorithms 整表 replace + WS 广播
//
// 优雅退出：signal → retransmitter → janitor → ws → hook → http → uds → db
#include <signal.h>

#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <csignal>
#include <cstdio>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <json/json.h>

#include "common/Log.h"
#include "common/NonCopyable.h"
#include "hook_server.h"
#include "http_api.h"
#include "janitor.h"
#include "uds_client.h"
#include "ws_server.h"
#include "zlm_client.h"

namespace aibox {
namespace {

std::atomic<bool> g_shutdown{false};
void on_sig(int) { g_shutdown.store(true); }

// ---- 配置（JSON；缺省值 = 详设 §5.1 端口约定）----
struct Config {
    int http_port = 80;
    int ws_port = 8081;
    int hook_port = 8090;
    std::string bind_host = "0.0.0.0";
    std::string uds_path = "/tmp/aibox-engine.sock";
    std::string db_path = "/var/lib/aibox/businessd.db";
    std::string static_dir;         // 空 = 不托管前端
    std::string recordings_dir;     // 空 = 不挂 /recordings
    std::string auth_token;         // 空 = 不鉴权
    std::string version = "0.1.0";
    int npu_cores = 2;
    int64_t npu_mem_total_kb = 2097152;
    int keep_days = 30;
    bool mqtt_enabled = false;      // V1 概要：接口就位，云对接后续里程碑
    // zlm
    std::string zlm_host = "127.0.0.1";
    int zlm_port = 80;
    std::string zlm_secret;
    int zlm_rtsp_port = 554;  // 事件快照回环拉流口（getSnap url 用）

    static Config from_file(const std::string& path) {
        Config c;
        std::ifstream f(path);
        if (!f) {
            LOGW("main", "配置文件 %s 打不开，使用缺省配置", path.c_str());
            return c;
        }
        Json::Value root;
        Json::CharReaderBuilder b;
        std::string errs;
        if (!Json::parseFromStream(b, f, &root, &errs)) {
            LOGE("main", "配置解析失败: %s", errs.c_str());
            std::exit(2);
        }
        c.http_port = root.get("http_port", c.http_port).asInt();
        c.ws_port = root.get("ws_port", c.ws_port).asInt();
        c.hook_port = root.get("hook_port", c.hook_port).asInt();
        c.bind_host = root.get("bind_host", c.bind_host).asString();
        c.uds_path = root.get("uds_path", c.uds_path).asString();
        c.db_path = root.get("db_path", c.db_path).asString();
        c.static_dir = root.get("static_dir", c.static_dir).asString();
        c.recordings_dir = root.get("recordings_dir", c.recordings_dir).asString();
        c.auth_token = root.get("auth_token", c.auth_token).asString();
        c.version = root.get("version", c.version).asString();
        c.npu_cores = root.get("npu_cores", c.npu_cores).asInt();
        c.npu_mem_total_kb = root.get("npu_mem_total_kb", c.npu_mem_total_kb).asInt64();
        c.keep_days = root.get("keep_days", c.keep_days).asInt();
        c.mqtt_enabled = root.get("mqtt_enabled", c.mqtt_enabled).asBool();
        if (root.isMember("zlm")) {
            const Json::Value& z = root["zlm"];
            c.zlm_host = z.get("host", c.zlm_host).asString();
            c.zlm_port = z.get("port", c.zlm_port).asInt();
            c.zlm_secret = z.get("secret", c.zlm_secret).asString();
            c.zlm_rtsp_port = z.get("rtsp_port", c.zlm_rtsp_port).asInt();
        }
        return c;
    }
};

// MQTT V1 占位发布器：始终失败（outbox 保留，云对接里程碑替换为 libmosquitto 实现）
class DisabledMqttPublisher : public biz::IMqttPublisher {
public:
    bool publish(const std::string&, const std::string&) override { return false; }
};

int run(const std::string& config_path) {
    const Config cfg = Config::from_file(config_path);
    LOGI("main", "businessd up: db=%s uds=%s http=%d ws=%d hook=%d", cfg.db_path.c_str(),
         cfg.uds_path.c_str(), cfg.http_port, cfg.ws_port, cfg.hook_port);

    // 2. Db
    biz::Db db(cfg.db_path);
    if (db.migrate() < 0) {
        LOGE("main", "db migrate 失败: %s", db.last_error().c_str());
        return 1;
    }

    // ZlmClient（getServerConfig 自检：防产线配置漂移，详设 §4.1）
    // 板上实证：ZLM 各版本 data 形态不一（对象/数组/其他）——自检是信息性动作，
    // 任何应答形态都不允许 crash（非对象形态触发 Json::LogicError abort）
    biz::ZlmClient zlm({cfg.zlm_host, cfg.zlm_port, cfg.zlm_secret, 3000});
    {
        Json::Value conf;
        if (zlm.get_server_config(&conf) && conf.isObject() &&
            conf.isMember("hook.enable") && conf["hook.enable"].isString()) {
            LOGI("main", "zlm getServerConfig ok（hook.enable=%s）",
                 conf["hook.enable"].asString().c_str());
        } else {
            LOGW("main", "zlm getServerConfig 形态异常/不可达（hook 自检跳过，不影响运行）");
        }
    }

    // 3. HookServer
    biz::HookServer::Hooks hook_hooks;
    hook_hooks.on_stream_changed = [&db](int64_t cam, bool online) {
        db.set_camera_online(cam, online);  // WS 徽标经 camera_status 事件由 engine 同步；
                                            // hook 是"权威显示"通道（架构 §4.3 双通道）
    };
    hook_hooks.on_record_mp4 = [&db](const biz::SegmentRow& seg) { db.insert_segment(seg); };
    biz::HookServer hook(std::move(hook_hooks), {"127.0.0.1", cfg.hook_port});
    if (!hook.start()) return 1;

    // 5. WsServer（对象先建：on_evt 闭包引用它；hooks 在 uds 构造后补挂再 start）
    biz::WsServer ws(biz::WsServer::Hooks{});

    // ---- 事件快照线程（方案 A：ZLM getSnap 回环截图，独立于 UDS 接收线程）----
    // 队列元素 {事件行 id, 摄像头 id, 事件 ts}；getSnap 走本机回环拉已有代理流
    // （cam{id}_sub 常开，首帧即时返回）。独立线程的原因：on_evt 在 UDS 接收
    // 线程同步执行，getSnap 最长 3s 会堵死事件分发 + ZlmClient 全局锁连锁阻塞
    // HttpApi（板上实证：同步截导致"不报警"）
    struct SnapJob {
        int64_t event_id;
        uint32_t cam;
        int64_t ts_ms;
    };
    std::mutex snap_mu;
    std::condition_variable snap_cv;
    std::deque<SnapJob> snap_q;
    std::atomic<bool> snap_running{true};
    std::thread snap_th([&] {
        const std::string snap_dir =
            cfg.recordings_dir.empty() ? "" : cfg.recordings_dir + "/snap";
        if (snap_dir.empty()) {
            // 可见性：recordings_dir 未配置 = 快照功能旁路（板上排障教训——
            // 静默丢弃任务会让"没快照"变成无头案）
            LOGW("main", "recordings_dir 未配置：事件快照功能旁路（事件照常入库）");
        } else if (::mkdir(snap_dir.c_str(), 0755) == 0) {
            LOGI("main", "snapshot dir ready: %s", snap_dir.c_str());
        }
        while (snap_running.load()) {
            SnapJob job;
            {
                std::unique_lock<std::mutex> lk(snap_mu);
                snap_cv.wait_for(lk, std::chrono::milliseconds(500),
                                 [&] { return !snap_q.empty(); });
                if (snap_q.empty()) continue;
                job = snap_q.front();
                snap_q.pop_front();
            }
            if (snap_dir.empty()) continue;  // 未配 recordings_dir：任务即弃
            char file[600];
            std::snprintf(file, sizeof(file), "%s/cam%lld_%lld.jpg", snap_dir.c_str(),
                          static_cast<long long>(job.cam),
                          static_cast<long long>(job.ts_ms));
            // 回环 URL：ZLM 自己的 RTSP 服务（代理流命名 cam{id}_sub，架构 §4.3）
            const std::string loop_rtsp =
                "rtsp://127.0.0.1:" + std::to_string(cfg.zlm_rtsp_port) +
                "/live/cam" + std::to_string(job.cam) + "_sub";
            std::string jpeg;
            if (zlm.get_snap(loop_rtsp, 3, &jpeg)) {
                std::ofstream f(file, std::ios::binary);
                if (f && f.write(jpeg.data(), static_cast<std::streamsize>(jpeg.size())) &&
                    f.good()) {
                    db.update_event_snapshot(job.event_id, file);
                    LOGI("main", "event %lld snapshot %s (%zu bytes)",
                         static_cast<long long>(job.event_id), file, jpeg.size());
                } else {
                    LOGW("main", "snapshot 落盘失败: %s", file);
                }
            } else {
                LOGW("main", "event %lld 截图失败（流可能已断，事件不丢）",
                     static_cast<long long>(job.event_id));
            }
        }
    });

    // 6. UDS 客户端（先于 HttpApi 构造：HttpApi::Deps 需要其指针；稍后 start）
    biz::UdsClient::Hooks uds_hooks;
    uds_hooks.ensure_proxies = [&db, &zlm]() -> bool {
        // ZLM 重启代理重建（详设 §5.7-5）：getMediaList 对比 DB，幂等逐台 ensure
        bool all_ok = true;
        for (const auto& cam : db.list_cameras(true)) {
            std::string km, ks;
            if (!zlm.add_camera_proxies(static_cast<int>(cam.id), cam.rtsp_main,
                                        cam.rtsp_sub, &km, &ks)) {
                all_ok = false;
                continue;
            }
            db.set_camera_keys(cam.id, km, ks);
        }
        return all_ok;
    };
    uds_hooks.on_evt = [&](const proto::ParsedMsg& pm, const std::string& line) {
        if (pm.name == "detections") {
            ws.send_meta(pm.detections.id, line);  // /ws/meta/{cam} 组播（原始行）
        } else if (pm.name == "event") {
            biz::EventRow e;
            e.camera_id = pm.event.id;
            e.algo_id = pm.event.algo;
            e.type = pm.event.type;
            e.ts = pm.event.ts;
            e.snapshot_path = pm.event.snap;
            // targets 从原始行提取（ParsedMsg 往返会丢未知字段）
            Json::Value root;
            Json::CharReaderBuilder rb;
            std::unique_ptr<Json::CharReader> rd(rb.newCharReader());
            std::string errs;
            if (rd->parse(line.data(), line.data() + line.size(), &root, &errs) &&
                root.isMember("targets")) {
                Json::StreamWriterBuilder wb;
                wb["indentation"] = "";
                e.targets_json = Json::writeString(wb, root["targets"]);
            }
            const int64_t id = db.insert_event(e);
            // 事件快照（方案 A）：入队到独立截图线程——on_evt 在 UDS 接收线程
            // 同步执行，getSnap 最长 3s（回环流断时超时）会堵死事件分发与
            // ZlmClient 全局锁（HttpApi 的 ZLM 操作连锁阻塞，板上实证"不报警"）
            if (id > 0) {
                std::lock_guard<std::mutex> lk(snap_mu);
                snap_q.push_back({id, pm.event.id, pm.event.ts});
            }
            ws.broadcast(line);  // WS 弹窗
            if (cfg.mqtt_enabled && id > 0) {
                // outbox 断网缓存：payload 附加 _event_id（重传 ack 后对账 reported=1）
                Json::Value payload = root;
                payload["_event_id"] = id;
                Json::StreamWriterBuilder wb;
                wb["indentation"] = "";
                db.insert_outbox("aibox/events/" + std::to_string(pm.event.id),
                                 Json::writeString(wb, payload));
            }
        } else if (pm.name == "camera_status") {
            db.set_camera_online(pm.camera_status.id, pm.camera_status.online);
            ws.broadcast(line);
        } else if (pm.name == "degrade") {
            LOGI("main", "degrade level=%d %s", pm.degrade.level,
                 pm.degrade.reason.c_str());
            ws.broadcast(line);
        } else if (pm.name == "registry_ready") {
            db.replace_algorithms(pm.registry_ready);  // 整表 replace（详设 §5.6）
            ws.broadcast(line);
        }
    };
    biz::UdsClient uds(cfg.uds_path, db, std::move(uds_hooks),
                       biz::UdsClient::Params{});  // 生产参数：1s 重连 / 5s ack / 5s 心跳

    // WS 订阅联动补挂（T4.5↔T4.7 关键接线，冒烟实证缺失则 detections 永不流通）：
    // viewer 首连 → sub_meta(engine)；全断 → unsub_meta。uds.start() 前完成，
    // 避免订阅竞态窗口
    ws.set_hooks({[&uds](uint32_t cam) { uds.sub_meta(cam, true); },
                  [&uds](uint32_t cam) { uds.sub_meta(cam, false); }});
    if (!ws.start(cfg.bind_host, cfg.ws_port)) return 1;

    // 4. HttpApi（uds 指针已就绪；先注册路由后 listen，start 前所有指针有效）
    biz::HttpApi::Deps api_deps;
    api_deps.db = &db;
    api_deps.uds = &uds;
    api_deps.zlm = &zlm;
    api_deps.http_port = cfg.http_port;
    api_deps.bind_host = cfg.bind_host;
    api_deps.static_dir = cfg.static_dir;
    api_deps.recordings_dir = cfg.recordings_dir;
    api_deps.auth_token = cfg.auth_token;
    api_deps.ws_port = cfg.ws_port;
    api_deps.npu_cores = cfg.npu_cores;
    api_deps.npu_mem_total_kb = cfg.npu_mem_total_kb;
    api_deps.version = cfg.version;
    biz::HttpApi api(std::move(api_deps));
    if (!api.start()) return 1;

    // 7. Janitor + Outbox 重传
    biz::DiskJanitor::Params jp;
    jp.mp4_dir = cfg.recordings_dir;
    jp.snap_dir = cfg.recordings_dir.empty() ? "" : cfg.recordings_dir + "/snap";
    jp.keep_days = cfg.keep_days;
    biz::DiskJanitor janitor(db, jp);  // recordings_dir 空 → 只跑保留天数（水位跳过）
    janitor.start();

    DisabledMqttPublisher disabled_pub;  // 本文件匿名命名空间定义（非 biz:: 成员）
    std::unique_ptr<biz::OutboxRetransmitter> tx;
    if (cfg.mqtt_enabled) {
        // V1：libmosquitto 实现属云对接里程碑；期间 outbox 只进不出（可见对账）
        LOGW("main", "mqtt_enabled=true 但 V1 未接 libmosquitto：outbox 仅缓存不重传");
        tx = std::make_unique<biz::OutboxRetransmitter>(
            db, static_cast<biz::IMqttPublisher*>(&disabled_pub));
        tx->start();
    }

    // 6（续）. UDS 启动（SYNCING 前 HttpApi 已可服务只读接口）
    uds.start();

    // 8. ready
    api.set_ready(true);
    LOGI("main", "businessd ready");

    // 事件循环
    while (!g_shutdown.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 优雅退出（反序）
    LOGI("main", "shutting down...");
    api.set_ready(false);
    uds.stop();
    snap_running.store(false);
    snap_cv.notify_all();
    if (snap_th.joinable()) snap_th.join();
    if (tx) tx->stop();
    janitor.stop();
    api.stop();
    ws.stop();
    hook.stop();
    LOGI("main", "bye");
    return 0;
}

}  // namespace
}  // namespace aibox

int main(int argc, char** argv) {
    using namespace aibox;
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    setvbuf(stdout, nullptr, _IOLBF, 0);

    std::string config_path = "businessd.conf";
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        if (s == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (s == "-h") {
            std::fprintf(stderr, "用法: businessd [--config businessd.conf]\n");
            return 0;
        } else {
            LOGE("main", "未知参数: %s", s.c_str());
            return 2;
        }
    }
    return run(config_path);
}
