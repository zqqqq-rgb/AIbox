// UDS 协议编解码（任务分解 T1.6，详设 §8.1 全字段表）
// 载体：JSON-lines（一对象一行，'\n' 分帧）；方向：
//   请求  businessd→engine：{"msg_id":u64, "cmd":"...", ...}，均需 ack
//   事件  engine→businessd：{"evt":"...", ...}，可丢
//   ack   engine→businessd：{"ack":<msg_id>, "ret":0, "data":...} / {"ack":..,"ret":E,"msg":".."}
// 错误码：0 成功；4001 协议错误（JSON 解析失败/缺字段/未知 cmd）——连接不断
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace aibox {
namespace proto {

constexpr int kOk = 0;
constexpr int kErrProtocol = 4001;  // 详设 §8.2

// ---- 请求载荷 ----
struct AddCam {
    uint32_t id = 0;
    std::string rtsp_sub;   // 回环子码流 URL
    std::string rtsp_main;  // 可选（P1 显示开启）
    bool has_rtsp_main = false;
};

struct AlgoCfg {
    std::string algo;
    float thr = 0.f;
    uint8_t fps = 0;
    bool has_roi = false;                      // roi 归一化 0~1，null = 全屏
    std::vector<std::pair<float, float>> roi;  // 多边形顶点 [[x,y],...]
    // 规则参数（事件接线：has_roi 时 engine 派生 RuleEngine 规则）；
    // decode 缺省保留默认值 → 新旧 businessd/engine 混跑向后兼容
    std::string rule_type = "region_in";  // region_in | dwell（V1 UI 只暴露这两类）
    std::string event_type;               // 空 = engine 按 algo_id+rule_type 兜底
    uint32_t dwell_sec = 5;               // DWELL 驻留阈值秒
    uint32_t debounce_sec = 5;            // 事件防抖窗秒
};

struct SetAlgos {
    uint32_t id = 0;
    std::vector<AlgoCfg> algos;
};

struct RemoveCam {
    uint32_t id = 0;
};

struct SubMeta {  // sub_meta / unsub_meta 共用（会话级，按 UDS 连接）
    uint32_t id = 0;
    bool subscribe = false;
};

struct ReloadAlgo {
    std::string algo;  // algo_id
};

struct GetMetrics {};

// ---- 事件载荷 ----
struct CamStatusEvt {
    uint32_t id = 0;
    bool online = false;
};

struct RegistryAlgo {  // registry_ready 的 algos[] 条目
    std::string algo_id, version, model_ref, trigger;
    float npu_cost_ms = 0.f;
    uint32_t npu_mem_kb = 0;
    bool disabled = false;
    // 类别白名单（COCO id，manifest.classes 原样透传；空 = 全类）——
    // businessd 落库 algorithms.classes，前端 /api/algorithms 显示标签用
    std::vector<int32_t> classes;
    // 类别名映射（manifest.class_names，下标即 COCO id；空 = 前端显示裸 id）
    std::vector<std::string> class_names;
};

struct RegistryReadyEvt {  // 启动/热升级/回滚后
    std::vector<RegistryAlgo> algos;
};

struct DetTarget {
    int32_t cls = 0;
    float conf = 0, x = 0, y = 0, w = 0, h = 0;  // 坐标 = 原始分辨率（详设 §12.5 契约）
    int32_t track_id = -1;
    float vx = 0, vy = 0;  // px/s（ByteTrack Kalman 外推）
};

struct DetectionsEvt {
    uint32_t id = 0;
    int64_t ts = 0;          // 墙钟 us（事件检索对齐录像）
    uint64_t frame_seq = 0;  // 抽帧计数
    int64_t frame_pts = 0;   // 归零 pts us（前端 pts 对齐，详设 §12.5）
    uint32_t frame_w = 0;    // 检测域分辨率 = 解码帧宽高（targets 坐标基准；
    uint32_t frame_h = 0;    //   前端 fitRect 缩放依据，主/子码流框对齐）
    std::vector<DetTarget> targets;
};

struct RuleEventEvt {  // RuleEngine 产出
    uint32_t id = 0;
    std::string algo, type, snap;  // snap = 快照路径（取证，非实时截图）
    int64_t ts = 0;
    std::vector<DetTarget> targets;
};

struct DegradeEvt {
    uint8_t level = 0;  // 0~5，0=全恢复
    std::string reason;
};

// ---- ack ----
struct Ack {
    uint64_t ack = 0;    // 对应请求 msg_id
    int ret = kOk;
    std::string msg;     // ret != 0 时
    std::string data;    // ret == 0 时可选：JSON 值的预序列化片段
                         //（wire 上为 §8.1 内联 JSON 对象，如 get_metrics 响应）
    bool has_data = false;
};

// ---- 编码 ----
std::string encode_request(uint64_t msg_id, const AddCam& v);
std::string encode_request(uint64_t msg_id, const SetAlgos& v);
std::string encode_request(uint64_t msg_id, const RemoveCam& v);
std::string encode_request(uint64_t msg_id, const SubMeta& v);  // 按 subscribe 选 sub/unsub
std::string encode_request(uint64_t msg_id, const ReloadAlgo& v);
std::string encode_request(uint64_t msg_id, const GetMetrics& v);

std::string encode_event(const CamStatusEvt& v);
std::string encode_event(const RegistryReadyEvt& v);
std::string encode_event(const DetectionsEvt& v);
std::string encode_event(const RuleEventEvt& v);
std::string encode_event(const DegradeEvt& v);

std::string encode_ack(const Ack& v);

// ---- 解码 ----
// 解析一行 JSON 到按名分发 union；一次只填充与 name 匹配的成员
struct ParsedMsg {
    std::string kind;  // "cmd" / "evt" / "ack"
    std::string name;  // cmd/evt 名；ack 时为空
    uint64_t msg_id = 0;

    AddCam add_cam;
    SetAlgos set_algos;
    RemoveCam remove_cam;
    SubMeta sub_meta;
    ReloadAlgo reload_algo;
    GetMetrics get_metrics;

    CamStatusEvt camera_status;
    RegistryReadyEvt registry_ready;
    DetectionsEvt detections;
    RuleEventEvt event;
    DegradeEvt degrade;

    Ack ack;
};

// 返回 kOk / kErrProtocol（坏 JSON、截断、未知 cmd/evt、缺必填字段——连接不断）
int decode_line(const std::string& line, ParsedMsg& out);

}  // namespace proto
}  // namespace aibox
