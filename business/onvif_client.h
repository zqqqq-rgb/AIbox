// OnvifClient —— 局域网摄像头发现与取流（M5 前端"扫描添加"后端支撑）
//
// 1. WS-Discovery 组播探测：UDP 239.255.255.250:3702 发 Probe（Types=NetworkVideoTransmitter），
//    收集 ProbeMatches 的 XAddrs（无鉴权，ONVIF 设备开机即响应）。
// 1b. HTTP 网段兜底（与组播并行）：对每个接口所在 /24 逐 IP 发
//    GetSystemDateAndTime（ONVIF 免鉴权规范调用）——专治关闭 discovery 但
//    ONVIF 服务在 80 口的相机（海康新固件默认关等）。64 并发 ~2s。
// 1c. RTSP 探测（同网段并行）：554 口 OPTIONS 免鉴权应答即 RTSP 设备；
//    顺带探 8000（海康 SDK 口）标 hik——与开源端口扫描工具发现能力对齐。
// 2. ONVIF SOAP 取流：GetProfiles → 前 2 个 Profile 各 GetStreamUri，得到主/子码流 RTSP。
//    鉴权：WS-UsernameToken PasswordDigest（ONVIF 规范强制；设备服务层
//    401/403 即账密错误 → 2002，不做传输层 digest 重试）
//
// 纯函数（digest/XML 提取/URL 账密注入）独立导出供单测（test_onvif）。
// 错误码约定（详设 §8.2 摄像头段）：2001 网络/协议失败，2002 账密错误。
#pragma once

#include <string>
#include <vector>

namespace aibox {
namespace biz {

// ---- 纯函数（单测覆盖）----

// WS-Security UsernameToken PasswordDigest =
//   base64( SHA1( nonce_raw + created + password ) )
// nonce_raw 为原始字节（非 base64）；created 为 ISO8601 UTC（如 2026-10-09T02:00:00Z）
std::string wsu_password_digest(const std::string& nonce_raw, const std::string& created,
                                const std::string& password);

// 提取首个 <[前缀:]localname> 的文本内容（跳过属性、跳过自闭合），
// 做 XML 基本实体解码（&amp; 等，取流 URI 的 & 常被转义）。找不到返回空串。
std::string xml_first_text(const std::string& xml, const std::string& localname);

// 提取所有 <[前缀:]localname ... attr="value"> 的属性值，按出现序去重
// （GetProfiles 响应中提取 Profile token 用）
std::vector<std::string> xml_tag_attrs(const std::string& xml, const std::string& localname,
                                       const std::string& attr);

// rtsp://host:port/path → rtsp://user:pass@host:port/path（已有账密则替换；
// user/pass 做 URL percent-encode，密码含 @:#& 等特殊字符安全）
std::string rtsp_insert_credentials(const std::string& uri, const std::string& user,
                                    const std::string& pass);

// ---- WS-Discovery 探测 ----

struct DiscoveredDevice {
    std::string ip;     // 响应报文源 IP / 探测命中 IP（去重键）
    std::string xaddr;  // onvif=Device Service 地址；rtsp=rtsp://ip:554
    std::string kind = "onvif";  // "onvif" | "rtsp"（rtsp=未开 ONVIF 但 554 有 RTSP 服务）
    bool hik = false;            // rtsp 设备且 8000（海康 SDK 口）开放 → 取流路径按海康预填
};

// 组播 Probe 并收集 timeout_ms 毫秒内的响应（期间每 500ms 重发一次）。
// 无设备/无网络返回空列表（不报错——扫描失败不该炸 REST）。
std::vector<DiscoveredDevice> ws_discovery_probe(int timeout_ms);

// ---- ONVIF 取流 ----

struct OnvifUris {
    std::string rtsp_main;  // 失败时为空
    std::string rtsp_sub;   // 设备只有一个 Profile 时与主码流相同
    int profiles = 0;
};

// 对设备（ip:port+path 取自 xaddr）执行 GetProfiles → GetStreamUri。
// code_out：0 成功 / 2001 网络、SOAP 或解析失败 / 2002 鉴权失败。
OnvifUris onvif_get_stream_uris(const std::string& ip, const std::string& xaddr,
                                const std::string& user, const std::string& pass,
                                int timeout_ms, int* code_out);

}  // namespace biz
}  // namespace aibox
