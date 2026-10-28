#include "onvif_client.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include <httplib.h>

#include "common/Log.h"

namespace aibox {
namespace biz {

namespace {

// ---- SHA1（RFC 3174 紧凑实现，与 ws_server.cpp 同源；仅 UsernameToken 用）----
inline uint32_t rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::vector<uint8_t> msg(data, data + len);
    const uint64_t bits = static_cast<uint64_t>(len) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>(bits >> (i * 8)));

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const size_t p = off + static_cast<size_t>(i) * 4;
            w[i] = (static_cast<uint32_t>(msg[p]) << 24) |
                   (static_cast<uint32_t>(msg[p + 1]) << 16) |
                   (static_cast<uint32_t>(msg[p + 2]) << 8) |
                   static_cast<uint32_t>(msg[p + 3]);
        }
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = static_cast<uint8_t>(h[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
    }
}

std::string base64(const uint8_t* p, size_t n) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(p[i]) << 16) |
                           (i + 1 < n ? static_cast<uint32_t>(p[i + 1]) << 8 : 0) |
                           (i + 2 < n ? static_cast<uint32_t>(p[i + 2]) : 0);
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(i + 1 < n ? tbl[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < n ? tbl[v & 63] : '=');
    }
    return out;
}

// ---- XML 局部辅助 ----

// 定位首个 <[前缀:]name 的开标签；begin 返回标签名之后的位置（指向属性或 '>'）
bool find_open_tag(const std::string& xml, const std::string& name, size_t* begin) {
    size_t pos = 0;
    while ((pos = xml.find('<', pos)) != std::string::npos) {
        const size_t s = ++pos;
        if (s >= xml.size()) return false;
        if (xml[s] == '/' || xml[s] == '!' || xml[s] == '?') continue;  // 闭/注释/PI
        size_t name_pos = std::string::npos;
        if (xml.compare(s, name.size(), name) == 0 &&
            (s + name.size() >= xml.size() || !std::isalnum(static_cast<unsigned char>(xml[s + name.size()])))) {
            name_pos = s;  // 无前缀（且不是更长标签名的前缀，如 Uri vs UriReference）
        } else {
            const size_t colon = xml.find(':', s);
            if (colon != std::string::npos && colon - s <= 16 &&
                xml.compare(colon + 1, name.size(), name) == 0 &&
                (colon + 1 + name.size() >= xml.size() ||
                 !std::isalnum(static_cast<unsigned char>(xml[colon + 1 + name.size()])))) {
                name_pos = colon + 1;
            }
        }
        if (name_pos != std::string::npos) {
            *begin = name_pos + name.size();
            return true;
        }
    }
    return false;
}

std::string xml_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out.push_back(s[i]); continue; }
        if (s.compare(i, 5, "&amp;") == 0) { out.push_back('&'); i += 4; }
        else if (s.compare(i, 4, "&lt;") == 0) { out.push_back('<'); i += 3; }
        else if (s.compare(i, 4, "&gt;") == 0) { out.push_back('>'); i += 3; }
        else if (s.compare(i, 6, "&quot;") == 0) { out.push_back('"'); i += 5; }
        else if (s.compare(i, 6, "&apos;") == 0) { out.push_back('\''); i += 5; }
        else out.push_back('&');
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// percent-encode（RFC 3986 unreserved 之外的字符全转义；账密注入 URI 用）
std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 2);
    for (const char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '-' || c == '.' || c == '_' || c == '~') {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(hex[u >> 4]);
            out.push_back(hex[u & 15]);
        }
    }
    return out;
}

// 解析 http://host[:port]/path
bool parse_http_url(const std::string& url, std::string* host, int* port, std::string* path) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return false;
    const size_t auth_begin = scheme + 3;
    // 去掉可能出现在 XAddr 里的 userinfo（罕见，防御）
    size_t auth_end = url.find('/', auth_begin);
    if (auth_end == std::string::npos) auth_end = url.size();
    std::string authority = url.substr(auth_begin, auth_end - auth_begin);
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        *host = authority.substr(0, colon);
        *port = std::atoi(authority.c_str() + colon + 1);
        if (*port <= 0 || *port > 65535) return false;
    } else {
        *host = authority;
        *port = 80;
    }
    *path = auth_end < url.size() ? url.substr(auth_end) : "/onvif/device_service";
    if (host->empty()) return false;
    return true;
}

// 当前时刻 ISO8601 UTC
std::string iso8601_utc_now() {
    const time_t t = time(nullptr);
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return buf;
}

// 16 字节随机 nonce
bool rand_bytes(uint8_t* out, size_t n) {
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    const size_t got = fread(out, 1, n, f);
    fclose(f);
    return got == n;
}

// SOAP 信封（带 WS-UsernameToken PasswordDigest）
std::string soap_envelope(const std::string& user, const std::string& pass,
                          const std::string& body) {
    const std::string created = iso8601_utc_now();
    uint8_t nonce[16];
    rand_bytes(nonce, sizeof(nonce));  // 失败也继续：弱 nonce 只影响单次会话
    const std::string nonce_raw(reinterpret_cast<const char*>(nonce), sizeof(nonce));
    const std::string nonce_b64 = base64(nonce, sizeof(nonce));
    const std::string digest = wsu_password_digest(nonce_raw, created, pass);

    // Username 需 XML 转义（& < >）
    std::string user_esc;
    for (const char c : user) {
        if (c == '&') user_esc += "&amp;";
        else if (c == '<') user_esc += "&lt;";
        else if (c == '>') user_esc += "&gt;";
        else user_esc.push_back(c);
    }

    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
           "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
           "<s:Header>"
           "<Security s:mustUnderstand=\"1\" "
           "xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\">"
           "<UsernameToken><Username>" + user_esc + "</Username>"
           "<Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd#PasswordDigest\">" +
           digest + "</Password>"
           "<Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd#Base64Binary\">" +
           nonce_b64 + "</Nonce><Created>" + created + "</Created>"
           "</UsernameToken></Security></s:Header>"
           "<s:Body>" + body + "</s:Body></s:Envelope>";
}

const char* kXmlHdr =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";

// POST 一条 SOAP（action=SOAPAction 完整 URI，海康等固件校验缺失即 500）。
// 返回 0 成功（out=响应体）；2001 连接/传输失败；2002 鉴权失败
// （板上 httplib 无 set_digest_auth；ONVIF 规范要求设备走 SOAP 层
// WS-UsernameToken 鉴权，传输层 401/403 即账密错误）
int soap_post(const std::string& host, int port, const std::string& path,
              const std::string& user, const std::string& pass,
              const std::string& body, const std::string& action,
              std::string* out) {
    auto cli = std::make_unique<httplib::Client>(host, port);
    cli->set_connection_timeout(3);
    cli->set_read_timeout(3);
    cli->set_write_timeout(3);

    const std::string payload = kXmlHdr + soap_envelope(user, pass, body);
    httplib::Headers hdrs;
    if (!action.empty()) hdrs.emplace("SOAPAction", "\"" + action + "\"");
    const char* ctype = "application/soap+xml; charset=utf-8";
    const auto r = cli->Post(path.c_str(), hdrs, payload, ctype);
    if (!r) return 2001;
    if (r->status == 401 || r->status == 403) return 2002;
    if (r->status != 200) return 2001;
    *out = r->body;
    return 0;
}

// Probe 报文（Types 限定 NetworkVideoTransmitter，过滤打印机/NVR 干扰）
// MessageID 按 WS-Discovery 规范必须是 URI（urn:uuid:...）：
// 部分厂商固件（海康/大华）对无 urn: 前缀的 MessageID 直接丢包不响应
std::string discovery_probe_msg(const std::string& msg_id) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
           "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
           "xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
           "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
           "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
           "<e:Header>"
           "<w:MessageID>urn:uuid:" + msg_id + "</w:MessageID>"
           "<w:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>"
           "<w:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action>"
           "</e:Header>"
           "<e:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></e:Body>"
           "</e:Envelope>";
}

// ---- HTTP 网段兜底探测 ----
// 场景：相机关闭 WS-Discovery（海康新固件默认关/阉割固件很常见），但 ONVIF
// HTTP 服务在 80 口。逐 IP 发 GetSystemDateAndTime（ONVIF 规范免鉴权调用）：
// 响应含 SystemDateAndTime 或 401 → 判定 ONVIF 设备。
const char* kProbeBody =
    "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
    "<s:Body><tds:GetSystemDateAndTime "
    "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"/></s:Body></s:Envelope>";

// 带超时 TCP connect（非阻塞 connect + poll，阻塞探测 254 个 IP 不可接受）
int connect_to(const std::string& ip, int port, int timeout_ms) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    a.sin_addr.s_addr = inet_addr(ip.c_str());
    if (connect(fd, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0) {
        pollfd p{fd, POLLOUT, 0};
        if (poll(&p, 1, timeout_ms) != 1) { close(fd); return -1; }
        int err = 0;
        socklen_t el = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err != 0) { close(fd); return -1; }
    }
    fcntl(fd, F_SETFL, flags);  // 恢复阻塞，复用 SO_RCVTIMEO 控收包
    const timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

// 单 IP ONVIF HTTP 探测；命中返回设备服务地址
bool http_probe_onvif(const std::string& ip, int timeout_ms, std::string* xaddr) {
    const int fd = connect_to(ip, 80, timeout_ms);
    if (fd < 0) return false;
    const std::string body = kProbeBody;
    std::string req = "POST /onvif/device_service HTTP/1.1\r\n";
    req += "Host: " + ip + "\r\n";
    req += "Content-Type: application/soap+xml; charset=utf-8\r\n";
    // 海康等固件校验 SOAPAction，缺失直接 500
    req += "SOAPAction: \"http://www.onvif.org/ver10/device/wsdl/GetSystemDateAndTime\"\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Connection: close\r\n\r\n";
    req += body;

    bool sent = true;
    size_t off = 0;
    while (off < req.size()) {
        const ssize_t n = send(fd, req.data() + off, req.size() - off, 0);
        if (n <= 0) { sent = false; break; }
        off += static_cast<size_t>(n);
    }
    std::string resp;
    if (sent) {
        char buf[2048];
        while (resp.size() < 8192) {
            const ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            resp.append(buf, buf + static_cast<size_t>(n));
        }
    }
    close(fd);
    if (!sent || resp.compare(0, 5, "HTTP/") != 0) return false;
    std::string low;
    low.reserve(resp.size());
    for (const char c : resp)
        low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    // 命中判据（任一）：
    //   systemdateandtime —— GetSystemDateAndTimeResponse 正常应答
    //   faultcode         —— SOAP fault（严格固件对无鉴权探测回 fault，
    //                        但能回 SOAP fault 的必是 ONVIF 服务；普通 web
    //                        服务对未知 POST 是 404/405 HTML）
    //   401 unauthorized  —— 严格鉴权固件
    if (low.find("systemdateandtime") == std::string::npos &&
        low.find("faultcode") == std::string::npos &&
        low.find("401 unauthorized") == std::string::npos) {
        return false;
    }
    *xaddr = "http://" + ip + "/onvif/device_service";
    return true;
}

// RTSP 探测：554 口发 OPTIONS（免鉴权），应答以 RTSP/ 开头 → RTSP 设备
// （200/400/401 均算——非 RTSP 的 TCP 服务不会说出 RTSP/ 协议头）。
// 命中后顺带探 8000（海康 SDK 控制口）→ hik 标记，前端据此预填海康取流路径
bool http_probe_rtsp(const std::string& ip, int timeout_ms, bool* hik) {
    const int fd = connect_to(ip, 554, timeout_ms);
    if (fd < 0) return false;
    // 完整 URI（板上实测 rtsp://* 野卡会被海康固件冷处理）
    const std::string req =
        "OPTIONS rtsp://" + ip + ":554/ RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: AIBox\r\n\r\n";
    bool sent = true;
    size_t off = 0;
    while (off < req.size()) {
        const ssize_t n = send(fd, req.data() + off, req.size() - off, 0);
        if (n <= 0) { sent = false; break; }
        off += static_cast<size_t>(n);
    }
    char buf[512] = {0};
    bool hit = false;
    if (sent) {
        const ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n > 0 && n < static_cast<ssize_t>(sizeof(buf))) buf[n] = 0;
        hit = strncmp(buf, "RTSP/", 5) == 0;
    }
    close(fd);
    if (!hit) return false;
    const int fd2 = connect_to(ip, 8000, 200);  // 海康 SDK 口快速判定
    if (fd2 >= 0) {
        *hik = true;
        close(fd2);
    }
    return true;
}

std::vector<DiscoveredDevice> http_subnet_sweep(
    const std::vector<std::pair<in_addr_t, in_addr_t>>& nets,  // (接口地址, 掩码)
    const std::vector<std::string>& self_ips, int per_ip_timeout_ms) {
    // 目标集：各接口 /24 切片 × 254（大掩码不扩，扫描时间可控）；自身 IP 先排除
    std::vector<std::string> targets;
    std::map<std::string, bool> tseen;
    for (const auto& ip : self_ips) tseen[ip] = true;
    for (const auto& net : nets) {
        const uint32_t base = ntohl(net.first) & 0xFFFFFF00u;
        for (uint32_t v = base + 1; v <= base + 254; ++v) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (v >> 24) & 255u,
                     (v >> 16) & 255u, (v >> 8) & 255u, v & 255u);
            const std::string s(buf);
            if (tseen[s]) continue;
            tseen[s] = true;
            targets.push_back(s);
        }
    }

    std::vector<DiscoveredDevice> out;
    std::mutex mu;
    std::atomic<size_t> idx{0};
    std::atomic<int> n_onvif{0}, n_rtsp{0}, n_rtsp_hik{0};
    // 工作线程：64 并发 × 500ms → /24 最坏 ~2s
    const int nthreads = 64;
    auto worker = [&]() {
        while (true) {
            const size_t i = idx.fetch_add(1);
            if (i >= targets.size()) return;
            std::string x;
            if (http_probe_onvif(targets[i], per_ip_timeout_ms, &x)) {
                std::lock_guard<std::mutex> lk(mu);
                out.push_back({targets[i], x, "onvif", false});
                ++n_onvif;
                continue;
            }
            // ONVIF 未命中 → RTSP 层（开源扫描工具同款能力）
            bool hik = false;
            if (http_probe_rtsp(targets[i], per_ip_timeout_ms, &hik)) {
                std::lock_guard<std::mutex> lk(mu);
                out.push_back({targets[i], "rtsp://" + targets[i] + ":554", "rtsp", hik});
                ++n_rtsp;
                if (hik) ++n_rtsp_hik;
            }
        }
    };
    std::vector<std::thread> pool;
    const int n = std::min<int>(nthreads, static_cast<int>(targets.size()));
    for (int i = 0; i < n; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    LOGI("onvif", "sweep: %zu targets, onvif=%d rtsp=%d (hik=%d)",
         targets.size(), n_onvif.load(), n_rtsp.load(), n_rtsp_hik.load());
    return out;
}

}  // namespace

// ---- 纯函数实现 ----

std::string wsu_password_digest(const std::string& nonce_raw, const std::string& created,
                                const std::string& password) {
    std::vector<uint8_t> buf(nonce_raw.begin(), nonce_raw.end());
    buf.insert(buf.end(), created.begin(), created.end());
    buf.insert(buf.end(), password.begin(), password.end());
    uint8_t mac[20];
    sha1(buf.data(), buf.size(), mac);
    return base64(mac, sizeof(mac));
}

std::string xml_first_text(const std::string& xml, const std::string& localname) {
    size_t begin = 0;
    if (!find_open_tag(xml, localname, &begin)) return "";
    // 自闭合 <Name/>：'>' 前为 '/'
    const size_t gt = xml.find('>', begin);
    if (gt == std::string::npos) return "";
    if (gt > begin && xml[gt - 1] == '/') return "";
    const size_t close = xml.find("</", gt);
    if (close == std::string::npos) return "";
    return xml_decode(trim(xml.substr(gt + 1, close - gt - 1)));
}

std::vector<std::string> xml_tag_attrs(const std::string& xml, const std::string& localname,
                                       const std::string& attr) {
    std::vector<std::string> out;
    std::map<std::string, bool> seen;
    size_t pos = 0;
    while (true) {
        size_t begin = 0;
        // 从 pos 继续找下一个同名开标签
        std::string rest = xml.substr(pos);
        if (!find_open_tag(rest, localname, &begin)) break;
        const size_t gt = rest.find('>', begin);
        if (gt == std::string::npos) break;
        const std::string tag = rest.substr(begin, gt - begin);
        pos += gt;  // 推进到 '>'，下一轮从其后继续找（自闭合也不会死循环）
        std::string pat = attr + "=\"";
        char quote = '"';
        size_t a = tag.find(pat);
        if (a == std::string::npos) {  // 单引号属性兜底
            pat = attr + "='";
            quote = '\'';
            a = tag.find(pat);
            if (a == std::string::npos) continue;
        }
        const size_t v0 = a + pat.size();
        const size_t v1 = tag.find(quote, v0);
        if (v1 == std::string::npos) continue;
        const std::string val = trim(tag.substr(v0, v1 - v0));
        if (!val.empty() && !seen[val]) {
            seen[val] = true;
            out.push_back(val);
        }
    }
    return out;
}

std::string rtsp_insert_credentials(const std::string& uri, const std::string& user,
                                    const std::string& pass) {
    const size_t scheme = uri.find("://");
    if (scheme == std::string::npos || user.empty()) return uri;
    const size_t auth_begin = scheme + 3;
    size_t auth_end = uri.find('/', auth_begin);
    if (auth_end == std::string::npos) auth_end = uri.size();
    std::string authority = uri.substr(auth_begin, auth_end - auth_begin);
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);  // 替换旧账密
    const std::string creds = url_encode(user) + ":" + url_encode(pass) + "@";
    return uri.substr(0, auth_begin) + creds + authority + uri.substr(auth_end);
}

// ---- WS-Discovery ----

std::vector<DiscoveredDevice> ws_discovery_probe(int timeout_ms) {
    std::vector<DiscoveredDevice> out;
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return out;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
    // 关闭组播环回：避免收到自己发出的 Probe（自身无 XAddrs 虽然也会被丢弃）
    uint8_t loop = 0;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    // 绑定 3702 + 加入组播组：部分设备把 ProbeMatches 组播回 239.255.255.250:3702
    // 而非单播回源端口——不绑端口/不加组则这类回包永远收不到。
    // 绑定失败（端口被占）仍继续：退化为仅收单播回包
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(3702);
    if (bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        addr.sin_port = 0;  // 退化为随机端口
        bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    }

    // 枚举全部 IPv4 网卡：每个接口都加组 + 发 Probe（多网卡盒子不漏网段；
    // 内核默认只从默认路由口发组播，相机不在该口时单发必然无响应）
    std::vector<in_addr_t> ifaces;
    std::vector<std::pair<in_addr_t, in_addr_t>> ifnets;  // (地址, 掩码)，HTTP 兜底用
    std::vector<std::string> self_ips;
    struct ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) == 0) {
        for (const struct ifaddrs* p = ifap; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
            const in_addr_t ia = reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr.s_addr;
            ifaces.push_back(ia);
            char ipbuf[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, p->ifa_addr, ipbuf, sizeof(ipbuf));
            self_ips.push_back(ipbuf);
            in_addr_t mask = 0;
            if (p->ifa_netmask && p->ifa_netmask->sa_family == AF_INET) {
                mask = reinterpret_cast<sockaddr_in*>(p->ifa_netmask)->sin_addr.s_addr;
            }
            ifnets.emplace_back(ia, mask);
            ip_mreq mreq{};
            mreq.imr_multiaddr.s_addr = inet_addr("239.255.255.250");
            mreq.imr_interface.s_addr = ia;
            setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
        }
        freeifaddrs(ifap);
    }

    // HTTP 网段兜底探测与组播收包并行（组播 3s 期间 /24 已扫完）
    std::vector<DiscoveredDevice> sweep_out;
    std::thread sweep_th;
    if (!ifnets.empty()) {
        sweep_th = std::thread([&ifnets, &self_ips, &sweep_out]() {
            sweep_out = http_subnet_sweep(ifnets, self_ips, 500);
        });
    }

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(3702);
    dst.sin_addr.s_addr = inet_addr("239.255.255.250");

    uint8_t rnd[8];
    rand_bytes(rnd, sizeof(rnd));
    char msg_id[40];
    snprintf(msg_id, sizeof(msg_id), "%02x%02x%02x%02x-%02x%02x-%02x%02x-0001",
             rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5], rnd[6], rnd[7]);
    const std::string probe = discovery_probe_msg(msg_id);

    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed_ms = [&t0]() {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count());
    };
    int last_send = -1000;
    std::map<std::string, bool> seen;

    while (elapsed_ms() < timeout_ms) {
        if (elapsed_ms() - last_send >= 500) {  // 周期重发（丢包容错）
            if (ifaces.empty()) {
                sendto(fd, probe.data(), probe.size(), 0,
                       reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
            } else {
                for (const in_addr_t ia : ifaces) {  // 每张网卡各发一份
                    in_addr ifc{};
                    ifc.s_addr = ia;
                    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifc, sizeof(ifc));
                    sendto(fd, probe.data(), probe.size(), 0,
                           reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
                }
            }
            last_send = elapsed_ms();
        }
        timeval tv{0, 100000};  // 100ms 收包窗口
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char buf[4096];
        sockaddr_in src{};
        socklen_t slen = sizeof(src);
        const ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                                   reinterpret_cast<sockaddr*>(&src), &slen);
        if (n <= 0) continue;
        buf[n] = 0;
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
        const std::string key(ip);
        if (seen[key]) continue;
        // XAddrs 可能空格分隔多个，取第一个 http 地址
        std::string xaddrs = xml_first_text(buf, "XAddrs");
        size_t x0 = xaddrs.find("http");
        if (x0 == std::string::npos) continue;
        size_t x1 = x0;
        while (x1 < xaddrs.size() && !std::isspace(static_cast<unsigned char>(xaddrs[x1]))) ++x1;
        seen[key] = true;
        out.push_back({key, xaddrs.substr(x0, x1 - x0)});
    }
    close(fd);
    // 合并 HTTP 兜底结果（组播已发现的 IP 去重）
    if (sweep_th.joinable()) sweep_th.join();
    for (const auto& d : sweep_out) {
        if (!seen[d.ip]) {
            seen[d.ip] = true;
            out.push_back(d);
        }
    }
    return out;
}

// ---- ONVIF 取流 ----

OnvifUris onvif_get_stream_uris(const std::string& ip, const std::string& xaddr,
                                const std::string& user, const std::string& pass,
                                int timeout_ms, int* code_out) {
    OnvifUris out;
    *code_out = 2001;
    (void)timeout_ms;  // 单请求超时固定 3s（连接/读/写各 3s 上限，全链最坏 ~9s）

    // host 用探测到的源 IP（XAddr host 在多网卡设备上常不可达），port/path 用 XAddr
    std::string xhost, xpath;
    int xport = 80;
    if (!parse_http_url(xaddr, &xhost, &xport, &xpath)) {
        *code_out = 2001;
        return out;
    }

    // 1. GetProfiles
    std::string resp;
    int rc = soap_post(ip, xport, xpath, user, pass,
                       "<trt:GetProfiles xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"/>",
                       "http://www.onvif.org/ver10/media/wsdl/GetProfiles", &resp);
    if (rc != 0) {
        *code_out = rc;
        return out;
    }
    const std::vector<std::string> tokens = xml_tag_attrs(resp, "Profile", "token");
    if (tokens.empty()) {
        *code_out = 2001;  // 响应异常（无 Profile）
        return out;
    }

    // 2. 前 2 个 Profile 各取一路流（0=主 1=子，厂商 Profile 顺序约定俗成）
    const size_t want = tokens.size() > 1 ? 2 : 1;
    std::string uris[2];
    for (size_t i = 0; i < want; ++i) {
        std::string body =
            "<trt:GetStreamUri xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\""
            " xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
            "<trt:StreamSetup><tt:Stream>RTP-Unicast</tt:Stream>"
            "<tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport></trt:StreamSetup>"
            "<trt:ProfileToken>" + tokens[i] + "</trt:ProfileToken></trt:GetStreamUri>";
        std::string r2;
        rc = soap_post(ip, xport, xpath, user, pass, body,
                       "http://www.onvif.org/ver10/media/wsdl/GetStreamUri", &r2);
        if (rc != 0) {
            *code_out = rc;
            return out;
        }
        uris[i] = xml_first_text(r2, "Uri");
        if (uris[i].empty()) {
            *code_out = 2001;
            return out;
        }
    }
    out.profiles = static_cast<int>(tokens.size());
    out.rtsp_main = rtsp_insert_credentials(uris[0], user, pass);
    out.rtsp_sub = want > 1 ? rtsp_insert_credentials(uris[1], user, pass) : out.rtsp_main;
    *code_out = 0;
    return out;
}

}  // namespace biz
}  // namespace aibox
