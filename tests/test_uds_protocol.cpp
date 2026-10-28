// T1.6 单测（任务分解 §4-T1.6）
// 1) 全消息类型往返 —— 结构→JSON→结构 逐字段相等
// 2) 坏 JSON / 截断 / 未知 cmd / 缺必填 —— 返回 4001（连接不断由 server 层保证，此处验码）
#include "ipc/Protocol.h"
#include "test_util.h"

using namespace aibox;
using namespace aibox::proto;

namespace {

template <typename F>
void roundtrip_request(uint64_t msg_id, const F& build, void (*verify)(const ParsedMsg&)) {
    const std::string line = build(msg_id);
    ParsedMsg out;
    CHECK(decode_line(line, out) == kOk);
    CHECK(out.kind == "cmd");
    CHECK(out.msg_id == msg_id);
    verify(out);
}

void run() {
    uint64_t mid = 100;

    // ---- 请求往返 ----
    roundtrip_request(mid++, [](uint64_t id) {
        AddCam v;
        v.id = 7;
        v.rtsp_sub = "rtsp://127.0.0.1:554/cam7_sub";
        v.has_rtsp_main = true;
        v.rtsp_main = "rtsp://127.0.0.1:554/cam7_main";
        return encode_request(id, v);
    }, [](const ParsedMsg& o) {
        CHECK(o.name == "add_cam");
        CHECK(o.add_cam.id == 7);
        CHECK(o.add_cam.rtsp_sub == "rtsp://127.0.0.1:554/cam7_sub");
        CHECK(o.add_cam.has_rtsp_main);
        CHECK(o.add_cam.rtsp_main == "rtsp://127.0.0.1:554/cam7_main");
    });

    roundtrip_request(mid++, [](uint64_t id) {
        AddCam v;  // rtsp_main 缺省
        v.id = 3;
        v.rtsp_sub = "rtsp://s";
        return encode_request(id, v);
    }, [](const ParsedMsg& o) {
        CHECK(o.name == "add_cam");
        CHECK(!o.add_cam.has_rtsp_main);
    });

    roundtrip_request(mid++, [](uint64_t id) {
        SetAlgos v;
        v.id = 7;
        AlgoCfg a;
        a.algo = "yolov8n_det";
        a.thr = 0.45f;
        a.fps = 3;
        a.has_roi = true;
        a.roi = {{0.1f, 0.2f}, {0.8f, 0.2f}, {0.5f, 0.9f}};
        a.rule_type = "dwell";
        a.event_type = "loiter";
        a.dwell_sec = 10;
        a.debounce_sec = 8;
        v.algos.push_back(a);
        AlgoCfg b;  // roi=null 全屏 + 规则字段缺省（event_type 空不编码）
        b.algo = "face_retina";
        b.thr = 0.6f;
        b.fps = 1;
        v.algos.push_back(b);
        return encode_request(id, v);
    }, [](const ParsedMsg& o) {
        CHECK(o.name == "set_algos");
        CHECK(o.set_algos.id == 7);
        CHECK(o.set_algos.algos.size() == 2);
        const AlgoCfg& a = o.set_algos.algos[0];
        CHECK(a.algo == "yolov8n_det");
        CHECK(a.thr == 0.45f);
        CHECK(a.fps == 3);
        CHECK(a.has_roi && a.roi.size() == 3);
        CHECK(a.roi[1].first == 0.8f && a.roi[1].second == 0.2f);
        // 规则参数往返（事件接线）
        CHECK(a.rule_type == "dwell");
        CHECK(a.event_type == "loiter");
        CHECK(a.dwell_sec == 10);
        CHECK(a.debounce_sec == 8);
        const AlgoCfg& b = o.set_algos.algos[1];
        CHECK(!b.has_roi);
        // 缺省字段 decode 后保持 AlgoCfg 默认值
        CHECK(b.rule_type == "region_in");
        CHECK(b.event_type.empty());
        CHECK(b.dwell_sec == 5);
        CHECK(b.debounce_sec == 5);
    });

    // 规则字段向后兼容：老版 wire 帧（无 rule_type 等）→ decode 保默认值不报错
    {
        ParsedMsg o;
        CHECK(decode_line(R"({"cmd":"set_algos","msg_id":1,"id":2,"algos":[
            {"algo":"person-det","thr":0.3,"fps":3,"roi":null}]})", o) == kOk);
        CHECK(o.set_algos.algos.size() == 1);
        CHECK(!o.set_algos.algos[0].has_roi);
        CHECK(o.set_algos.algos[0].rule_type == "region_in");
        CHECK(o.set_algos.algos[0].dwell_sec == 5);
        CHECK(o.set_algos.algos[0].debounce_sec == 5);
    }

    roundtrip_request(mid++, [](uint64_t id) {
        RemoveCam v;
        v.id = 9;
        return encode_request(id, v);
    }, [](const ParsedMsg& o) { CHECK(o.name == "remove_cam" && o.remove_cam.id == 9); });

    roundtrip_request(mid++, [](uint64_t id) {
        SubMeta v;
        v.id = 9;
        v.subscribe = true;
        return encode_request(id, v);
    }, [](const ParsedMsg& o) { CHECK(o.name == "sub_meta" && o.sub_meta.subscribe); });

    roundtrip_request(mid++, [](uint64_t id) {
        SubMeta v;
        v.id = 9;
        v.subscribe = false;
        return encode_request(id, v);
    }, [](const ParsedMsg& o) { CHECK(o.name == "unsub_meta" && !o.sub_meta.subscribe); });

    roundtrip_request(mid++, [](uint64_t id) {
        return encode_request(id, ReloadAlgo{"face_retina"});
    }, [](const ParsedMsg& o) { CHECK(o.name == "reload_algo" && o.reload_algo.algo == "face_retina"); });

    roundtrip_request(mid++, [](uint64_t id) {
        return encode_request(id, GetMetrics{});
    }, [](const ParsedMsg& o) { CHECK(o.name == "get_metrics"); });

    // ---- 事件往返 ----
    {
        CamStatusEvt v;
        v.id = 7;
        v.online = true;
        ParsedMsg o;
        CHECK(decode_line(encode_event(v), o) == kOk);
        CHECK(o.kind == "evt" && o.name == "camera_status");
        CHECK(o.camera_status.id == 7 && o.camera_status.online);
    }
    {
        RegistryReadyEvt v;
        RegistryAlgo a;
        a.algo_id = "yolov8n_det";
        a.version = "1.2.0";
        a.model_ref = "/opt/aibox/models/yolov8n.rknn";
        a.npu_cost_ms = 8.5f;
        a.npu_mem_kb = 20480;
        a.trigger = "always";
        a.disabled = false;
        v.algos.push_back(a);
        ParsedMsg o;
        CHECK(decode_line(encode_event(v), o) == kOk);
        CHECK(o.name == "registry_ready");
        CHECK(o.registry_ready.algos.size() == 1);
        CHECK(o.registry_ready.algos[0].npu_cost_ms == 8.5f);
        CHECK(o.registry_ready.algos[0].npu_mem_kb == 20480);
        CHECK(o.registry_ready.algos[0].model_ref == "/opt/aibox/models/yolov8n.rknn");
    }
    {
        DetectionsEvt v;
        v.id = 7;
        v.ts = 1759000000123456789LL;  // int64 边界值域内
        v.frame_seq = 123456789ULL;
        v.frame_pts = 9973333;  // 归零 pts us
        v.frame_w = 640;        // 检测域分辨率（前端 fitRect 缩放依据）
        v.frame_h = 480;
        DetTarget t;
        t.cls = 1;
        t.conf = 0.87f;
        t.x = 100.5f;
        t.y = 200.25f;
        t.w = 64.f;
        t.h = 128.f;
        t.track_id = 42;
        t.vx = 12.5f;
        t.vy = -3.25f;
        v.targets.push_back(t);
        ParsedMsg o;
        CHECK(decode_line(encode_event(v), o) == kOk);
        CHECK(o.name == "detections");
        CHECK(o.detections.frame_pts == 9973333);
        CHECK(o.detections.frame_seq == 123456789ULL);
        CHECK(o.detections.frame_w == 640);
        CHECK(o.detections.frame_h == 480);
        CHECK(o.detections.targets.size() == 1);
        const DetTarget& r = o.detections.targets[0];
        CHECK(r.cls == 1 && r.track_id == 42);
        CHECK(r.conf == 0.87f && r.vx == 12.5f && r.vy == -3.25f);
        CHECK(r.x == 100.5f && r.y == 200.25f && r.w == 64.f && r.h == 128.f);
    }
    {
        RuleEventEvt v;
        v.id = 7;
        v.algo = "intrusion";
        v.type = "enter_roi";
        v.ts = 1759000000123456789LL;
        v.snap = "/data/snap/7_20260929_153000.jpg";
        ParsedMsg o;
        CHECK(decode_line(encode_event(v), o) == kOk);
        CHECK(o.name == "event");
        CHECK(o.event.type == "enter_roi");
        CHECK(o.event.snap == "/data/snap/7_20260929_153000.jpg");
    }
    {
        DegradeEvt v;
        v.level = 3;
        v.reason = "npu_temp_85c";
        ParsedMsg o;
        CHECK(decode_line(encode_event(v), o) == kOk);
        CHECK(o.name == "degrade");
        CHECK(o.degrade.level == 3);
        CHECK(o.degrade.reason == "npu_temp_85c");
    }

    // ---- ack 往返（data 为 §8.1 内联 JSON 对象，非字符串） ----
    {
        Ack a;
        a.ack = 100;
        a.ret = kOk;
        a.has_data = true;
        a.data = R"({"channels":16,"degrade":0})";
        ParsedMsg o;
        CHECK(decode_line(encode_ack(a), o) == kOk);
        CHECK(o.kind == "ack" && o.ack.ack == 100 && o.ack.ret == 0);
        // wire 上是内联对象；decode 预序列化回 string，键序可能重排，按内容断言
        CHECK(o.ack.has_data);
        CHECK(o.ack.data.find("\"channels\":16") != std::string::npos);
        CHECK(o.ack.data.find("\"degrade\":0") != std::string::npos);
        CHECK(o.ack.data[0] == '{');  // 是对象不是字符串承载

        Ack e;
        e.ack = 101;
        e.ret = 1002;
        e.msg = "cam not found";
        CHECK(decode_line(encode_ack(e), o) == kOk);
        CHECK(o.ack.ack == 101 && o.ack.ret == 1002 && o.ack.msg == "cam not found");
        CHECK(!o.ack.has_data);
    }

    // ---- 坏输入 → 4001 ----
    {
        ParsedMsg o;
        CHECK(decode_line("not json at all", o) == kErrProtocol);
        CHECK(decode_line("{\"cmd\":\"add_cam\"", o) == kErrProtocol);        // 截断
        CHECK(decode_line("[]", o) == kErrProtocol);                          // 非对象
        CHECK(decode_line("{\"foo\":1}", o) == kErrProtocol);                 // 无 cmd/evt/ack
        CHECK(decode_line("{\"msg_id\":1,\"cmd\":\"no_such\"}", o) == kErrProtocol);   // 未知 cmd
        CHECK(decode_line("{\"evt\":\"no_such\"}", o) == kErrProtocol);       // 未知 evt
        CHECK(decode_line("{\"cmd\":\"add_cam\",\"msg_id\":1}", o) == kErrProtocol);   // 缺 rtsp_sub
        CHECK(decode_line("{\"cmd\":\"remove_cam\",\"msg_id\":1,\"id\":\"x\"}", o) ==
              kErrProtocol);                                                  // 类型错
        CHECK(decode_line("{\"evt\":\"detections\",\"id\":1,\"ts\":1,\"frame_seq\":1,"
                          "\"frame_pts\":1,\"frame_w\":640,\"frame_h\":480,"
                          "\"targets\":[{\"cls\":0}]}",
                          o) == kErrProtocol);                                // target 缺字段
        CHECK(decode_line("{\"evt\":\"detections\",\"id\":1,\"ts\":1,\"frame_seq\":1,"
                          "\"frame_pts\":1,\"targets\":[]}",
                          o) == kErrProtocol);                                // 缺 frame_w/frame_h
        CHECK(decode_line("{\"ack\":5}", o) == kErrProtocol);                 // ack 缺 ret
    }
}

}  // namespace

TEST_MAIN("test_uds_protocol")
