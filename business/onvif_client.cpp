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
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <set>
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
// 规范响应确认协议；401/403 或 SOAP fault 仅列为待认证候选。
const char* kProbeBody =
    "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
    "<s:Body><tds:GetSystemDateAndTime "
    "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"/></s:Body></s:Envelope>";

// All probe sockets remain nonblocking; one steady-clock deadline bounds connect,
// partial sends and trickle responses, even when peers keep a socket open.
using ScanClock = std::chrono::steady_clock;
using ScanDeadline = ScanClock::time_point;

struct IpRange {
    uint32_t base = 0, end = 0;
    int prefix = 0;
};
bool parse_ipv4(const std::string& value, uint32_t& result) {
    in_addr addr{};
    if (inet_pton(AF_INET, value.c_str(), &addr) != 1) return false;
    result = ntohl(addr.s_addr);
    return true;
}
std::string ipv4_string(uint32_t value) {
    in_addr addr{}; addr.s_addr = htonl(value);
    char text[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &addr, text, sizeof(text))) return {};
    return text;
}
uint32_t prefix_mask(int prefix) {
    return prefix == 0 ? 0u : (0xFFFFFFFFu << (32 - prefix));
}
bool mask_prefix(const std::string& text, int& prefix) {
    uint32_t mask = 0;
    if (!parse_ipv4(text, mask)) return false;
    prefix = 0;
    while (prefix < 32 && (mask & (uint32_t{1} << (31 - prefix)))) ++prefix;
    return prefix > 0 && mask == prefix_mask(prefix);
}
bool parse_cidr(const std::string& text, IpRange& range) {
    const auto slash = text.find('/');
    if (slash == std::string::npos || text.find('/', slash + 1) != std::string::npos) return false;
    uint32_t ip = 0;
    if (!parse_ipv4(text.substr(0, slash), ip)) return false;
    const auto suffix = text.substr(slash + 1);
    if (suffix.empty() || suffix.size() > 2) return false;
    int prefix = 0;
    for (char c : suffix) {
        if (c < '0' || c > '9') return false;
        prefix = prefix * 10 + (c - '0');
    }
    if (prefix < 1 || prefix > 32) return false;
    const uint32_t mask = prefix_mask(prefix);
    range = {ip & mask, (ip & mask) | ~mask, prefix};
    return true;
}
std::string cidr_string(const IpRange& range) {
    return ipv4_string(range.base) + "/" + std::to_string(range.prefix);
}
bool ordinary_ipv4(uint32_t value) {
    const uint32_t first = value >> 24;
    return first != 0 && first != 127 && first < 224;
}
int remaining_ms(ScanDeadline deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - ScanClock::now()).count();
    return left > 0 ? static_cast<int>(left) : 0;
}
bool wait_socket(int fd, short events, ScanDeadline deadline) {
    while (remaining_ms(deadline) > 0) {
        pollfd p{fd, events, 0};
        const int ready = poll(&p, 1, remaining_ms(deadline));
        if (ready > 0) return (p.revents & events) != 0;
        if (ready == 0 || errno != EINTR) return false;
    }
    return false;
}
int connect_to(const std::string& ip, int port, const std::string& source, ScanDeadline deadline) {
    if (!remaining_ms(deadline)) return -1;
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_in local{}; local.sin_family = AF_INET;
    if (!source.empty()) {
        inet_pton(AF_INET, source.c_str(), &local.sin_addr);
        if (bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            close(fd); return -1;
        }
    }
    sockaddr_in peer{}; peer.sin_family = AF_INET;
    peer.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, ip.c_str(), &peer.sin_addr);
    if (connect(fd, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) != 0) {
        if (errno != EINPROGRESS || !wait_socket(fd, POLLOUT, deadline)) { close(fd); return -1; }
        int err = 0; socklen_t length = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &length) != 0 || err) { close(fd); return -1; }
    }
    return fd;
}
std::string probe_exchange(const std::string& ip, int port, const std::string& source,
                           const std::string& request, ScanDeadline deadline) {
    const int fd = connect_to(ip, port, source, deadline);
    if (fd < 0) return {};
    size_t offset = 0;
    while (offset < request.size() && remaining_ms(deadline) > 0) {
        const ssize_t n = send(fd, request.data() + offset, request.size() - offset, MSG_NOSIGNAL);
        if (n > 0) offset += static_cast<size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && wait_socket(fd, POLLOUT, deadline)) continue;
        else break;
    }
    std::string response;
    if (offset == request.size()) {
        char buf[2048];
        while (response.size() < 8192 && remaining_ms(deadline) > 0) {
            const ssize_t n = recv(fd, buf, std::min(sizeof(buf), size_t{8192} - response.size()), 0);
            if (n > 0) {
                response.append(buf, static_cast<size_t>(n));
                // An RTSP status line is sufficient; HTTP may need the SOAP body.
                if (port == 554 && response.find("\r\n") != std::string::npos) break;
            } else if (n == 0) break;
            else if (errno == EINTR) continue;
            else if ((errno == EAGAIN || errno == EWOULDBLOCK) && wait_socket(fd, POLLIN, deadline)) continue;
            else break;
        }
    }
    close(fd);
    return response;
}
std::string target_source(const DiscoveryPlan& plan, const std::string& target) {
    uint32_t ip = 0; parse_ipv4(target, ip);
    for (const auto& entry : plan.interfaces) {
        uint32_t local = 0; int prefix = 0;
        if (parse_ipv4(entry.address, local) && mask_prefix(entry.netmask, prefix) &&
            (local & prefix_mask(prefix)) == (ip & prefix_mask(prefix))) return entry.address;
    }
    return {};
}
void probe_target(const std::string& ip, const std::string& source, ScanDeadline deadline,
                   std::vector<DiscoveredDevice>& devices) {
    const std::string body = kProbeBody;
    const std::string request = "POST /onvif/device_service HTTP/1.1\r\nHost: " + ip +
        "\r\nContent-Type: application/soap+xml; charset=utf-8\r\n"
        "SOAPAction: \"http://www.onvif.org/ver10/device/wsdl/GetSystemDateAndTime\"\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    bool confirmed = false;
    const auto response = probe_exchange(ip, 80, source, request,
        std::min(deadline, ScanClock::now() + std::chrono::milliseconds(250)));
    const bool candidate = classify_onvif_probe_response(response, confirmed);
    if (confirmed) {
        devices.push_back({ip, "http://" + ip + "/onvif/device_service", "onvif", false, "http_onvif", true});
        return;
    }
    // A generic web service returning 401 is only a candidate. Prefer a verified RTSP response.
    const auto rtsp = probe_exchange(ip, 554, source,
        "OPTIONS rtsp://" + ip + ":554/ RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: AIBox\r\n\r\n",
        std::min(deadline, ScanClock::now() + std::chrono::milliseconds(200)));
    if (rtsp.compare(0, 5, "RTSP/") == 0) {
        const int sdk = connect_to(ip, 8000, source,
            std::min(deadline, ScanClock::now() + std::chrono::milliseconds(100)));
        const bool hik = sdk >= 0;
        if (sdk >= 0) close(sdk);
        devices.push_back({ip, "rtsp://" + ip + ":554", "rtsp", hik, "rtsp", true});
    } else if (candidate) {
        devices.push_back({ip, "http://" + ip + "/onvif/device_service", "onvif", false, "http_candidate", false});
    }
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

bool classify_onvif_probe_response(const std::string& response, bool& confirmed) {
    confirmed = false;
    if (response.compare(0, 5, "HTTP/") != 0) return false;
    const auto header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) return false;
    std::string lower = response;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto body = lower.substr(header_end + 4);
    confirmed = body.find("getsystemdateandtimeresponse") != std::string::npos &&
                body.find("http://www.onvif.org/ver10/") != std::string::npos;
    const auto status_end = lower.find("\r\n");
    const auto status = lower.substr(0, status_end);
    const bool auth = status.find(" 401 ") != std::string::npos || status.find(" 403 ") != std::string::npos;
    const bool fault = body.find("faultcode") != std::string::npos ||
                       body.find(":fault>") != std::string::npos || body.find("<fault>") != std::string::npos;
    return confirmed || auth || fault;
}

std::vector<DiscoveryInterface> enumerate_discovery_interfaces() {
    std::vector<DiscoveryInterface> result;
    struct ifaddrs* addresses = nullptr;
    if (getifaddrs(&addresses) != 0) return result;
    std::set<std::string> seen;
    for (const auto* p = addresses; p; p = p->ifa_next) {
        if (!p->ifa_name || !p->ifa_addr || !p->ifa_netmask || p->ifa_addr->sa_family != AF_INET ||
            p->ifa_netmask->sa_family != AF_INET || !(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
        const auto ip = ntohl(reinterpret_cast<const sockaddr_in*>(p->ifa_addr)->sin_addr.s_addr);
        const auto mask = ntohl(reinterpret_cast<const sockaddr_in*>(p->ifa_netmask)->sin_addr.s_addr);
        DiscoveryInterface entry{p->ifa_name, ipv4_string(ip), ipv4_string(mask), {}};
        int prefix = 0;
        if (!ordinary_ipv4(ip) || !mask_prefix(entry.netmask, prefix)) continue;
        entry.cidr = cidr_string({ip & mask, (ip & mask) | ~mask, prefix});
        if (seen.insert(entry.name + "@" + entry.address).second) result.push_back(std::move(entry));
    }
    freeifaddrs(addresses);
    return result;
}

bool plan_discovery_targets(const std::vector<DiscoveryInterface>& interfaces,
                            const DiscoveryOptions& options, DiscoveryPlan& plan,
                            std::string& error) {
    plan = {}; error.clear();
    auto fail = [&](const std::string& message) { error = message; plan = {}; return false; };
    if (options.timeout_ms < 500 || options.timeout_ms > 10000) return fail("timeout_ms must be between 500 and 10000");
    if (!options.cidr.empty() && options.interface_name.empty()) return fail("select an interface before specifying a CIDR");
    IpRange requested;
    if (!options.cidr.empty() && !parse_cidr(options.cidr, requested)) return fail("invalid IPv4 CIDR");
    std::set<uint32_t> self;
    for (const auto& entry : interfaces) {
        uint32_t ip = 0;
        if (parse_ipv4(entry.address, ip)) self.insert(ip);
    }
    std::set<uint32_t> seen_targets;
    std::set<std::string> seen_interfaces, seen_ranges;
    bool interface_found = false;
    for (auto entry : interfaces) {
        if (!options.interface_name.empty() && entry.name != options.interface_name) continue;
        interface_found = true;
        uint32_t ip = 0; int prefix = 0;
        if (entry.name.empty() || !parse_ipv4(entry.address, ip) || !ordinary_ipv4(ip) || !mask_prefix(entry.netmask, prefix))
            return fail("invalid local interface address or netmask");
        const uint32_t mask = prefix_mask(prefix);
        const IpRange subnet{ip & mask, (ip & mask) | ~mask, prefix};
        IpRange range;
        if (!options.cidr.empty()) {
            if (requested.base < subnet.base || requested.end > subnet.end) continue;
            range = requested;
        } else {
            const int scan_prefix = std::max(24, prefix);
            const auto scan_mask = prefix_mask(scan_prefix);
            range = {ip & scan_mask, (ip & scan_mask) | ~scan_mask, scan_prefix};
        }
        if (!ordinary_ipv4(range.base) || !ordinary_ipv4(range.end)) return fail("CIDR must contain unicast IPv4 addresses");
        entry.cidr = cidr_string(subnet);
        if (seen_interfaces.insert(entry.name + "@" + entry.address).second) plan.interfaces.push_back(std::move(entry));
        if (seen_ranges.insert(cidr_string(range)).second) plan.cidrs.push_back(cidr_string(range));
        // /31 and /32 are valid point-to-point/host ranges; other ranges omit network/broadcast.
        const uint64_t begin = uint64_t(range.base) + (range.prefix < 31 ? 1 : 0);
        const uint64_t end = uint64_t(range.end) - (range.prefix < 31 ? 1 : 0);
        if (end >= begin && end - begin + 1 > 1024 + self.size()) return fail("scan exceeds 1024 candidates; select a smaller CIDR");
        for (uint64_t value = begin; value <= end; ++value) {
            const auto target = static_cast<uint32_t>(value);
            if (self.count(target) || (prefix < 31 && (target == subnet.base || target == subnet.end))) continue;
            if (seen_targets.insert(target).second) {
                if (seen_targets.size() > 1024) return fail("scan exceeds 1024 candidates; select an interface or smaller CIDR");
                plan.targets.push_back(ipv4_string(target));
            }
        }
    }
    if (!interface_found) return fail(options.interface_name.empty() ? "no active IPv4 interface" : "interface not found");
    if (plan.interfaces.empty()) return fail("CIDR is outside the selected interface's directly connected subnets");
    return true;
}

DiscoveryResult discover_cameras(const DiscoveryOptions& options) {
    DiscoveryResult result;
    const auto started = ScanClock::now();
    DiscoveryPlan plan;
    if (!plan_discovery_targets(enumerate_discovery_interfaces(), options, plan, result.message)) {
        result.code = 1001; return result;
    }
    const auto deadline = started + std::chrono::milliseconds(options.timeout_ms);
    result.target_count = plan.targets.size();
    std::atomic<size_t> next{0}, attempted{0}, completed{0};
    std::mutex found_mutex;
    std::vector<DiscoveredDevice> found;
    std::vector<std::thread> workers;
    auto worker = [&] {
        while (remaining_ms(deadline) > 0) {
            const auto index = next.fetch_add(1);
            if (index >= plan.targets.size()) break;
            attempted.fetch_add(1);
            std::vector<DiscoveredDevice> devices;
            probe_target(plan.targets[index], target_source(plan, plan.targets[index]), deadline, devices);
            if (remaining_ms(deadline) > 0) completed.fetch_add(1);
            if (!devices.empty()) {
                std::lock_guard<std::mutex> lock(found_mutex);
                found.insert(found.end(), devices.begin(), devices.end());
            }
        }
    };
    try {
        for (size_t i = 0; i < std::min(size_t{32}, plan.targets.size()); ++i) workers.emplace_back(worker);
    } catch (const std::exception&) {
        result.warnings.push_back("worker limit reached; scan continued with fewer workers");
    }

    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    std::set<std::string> allowed(plan.targets.begin(), plan.targets.end());
    std::set<std::string> discovered_sources;
    if (fd >= 0) {
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        uint8_t loop = 0;
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
        sockaddr_in local{}; local.sin_family = AF_INET; local.sin_port = htons(3702);
        if (bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            local.sin_port = 0;
            if (bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
                result.warnings.push_back("WS-Discovery receive bind failed");
        }
        for (const auto& entry : plan.interfaces) {
            ip_mreq group{}; group.imr_multiaddr.s_addr = inet_addr("239.255.255.250");
            inet_pton(AF_INET, entry.address.c_str(), &group.imr_interface);
            if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group, sizeof(group)) != 0)
                result.warnings.push_back("multicast unavailable on " + entry.name);
        }
        uint8_t bytes[16]{};
        const bool random_ok = rand_bytes(bytes, sizeof(bytes));
        if (!random_ok) result.warnings.push_back("WS-Discovery random source unavailable; multicast skipped");
        char id[48]{};
        std::snprintf(id, sizeof(id), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5],bytes[6],bytes[7],
            bytes[8],bytes[9],bytes[10],bytes[11],bytes[12],bytes[13],bytes[14],bytes[15]);
        const auto probe = discovery_probe_msg(id);
        sockaddr_in multicast{}; multicast.sin_family = AF_INET;
        multicast.sin_port = htons(3702); multicast.sin_addr.s_addr = inet_addr("239.255.255.250");
        auto last_send = started - std::chrono::milliseconds(500);
        while (random_ok && remaining_ms(deadline) > 0) {
            const auto now = ScanClock::now();
            if (now - last_send >= std::chrono::milliseconds(500)) {
                for (const auto& entry : plan.interfaces) {
                    in_addr source{}; inet_pton(AF_INET, entry.address.c_str(), &source);
                    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &source, sizeof(source)) == 0)
                        sendto(fd, probe.data(), probe.size(), 0, reinterpret_cast<const sockaddr*>(&multicast), sizeof(multicast));
                }
                last_send = now;
            }
            if (!wait_socket(fd, POLLIN, std::min(deadline, now + std::chrono::milliseconds(100)))) continue;
            char buffer[8192]; sockaddr_in peer{}; socklen_t peer_size = sizeof(peer);
            const auto size = recvfrom(fd, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&peer), &peer_size);
            if (size <= 0) continue;
            const std::string xml(buffer, static_cast<size_t>(size));
            const auto ip = ipv4_string(ntohl(peer.sin_addr.s_addr));
            if (!allowed.count(ip) || discovered_sources.count(ip) || xml.find("ProbeMatches") == std::string::npos ||
                xml_first_text(xml, "RelatesTo") != std::string("urn:uuid:") + id) continue;
            const auto addresses = xml_first_text(xml, "XAddrs");
            const auto begin = addresses.find("http://");
            if (begin == std::string::npos) continue;
            const auto end = addresses.find_first_of(" \t\r\n", begin);
            std::string host, path; int port = 80;
            if (!parse_http_url(addresses.substr(begin, end - begin), &host, &port, &path) ||
                path.find_first_of("\r\n") != std::string::npos) continue;
            const auto xaddr = "http://" + ip + (port == 80 ? "" : ":" + std::to_string(port)) + path;
            result.devices.push_back({ip, xaddr, "onvif", false, "ws_discovery", true});
            discovered_sources.insert(ip);
        }
        close(fd);
    } else {
        result.warnings.push_back("WS-Discovery socket unavailable; unicast probes only");
    }
    for (auto& thread : workers) thread.join();
    // Prefer confirmed ONVIF discovery, then confirmed RTSP, then unauthenticated candidates.
    std::map<std::string, DiscoveredDevice> unique;
    auto rank = [](const DiscoveredDevice& device) {
        return device.source == "ws_discovery" ? 4 : device.source == "http_onvif" ? 3 : device.source == "rtsp" ? 2 : 1;
    };
    for (const auto& device : result.devices) unique[device.ip] = device;
    for (const auto& device : found) {
        auto it = unique.find(device.ip);
        if (it == unique.end() || rank(device) > rank(it->second)) unique[device.ip] = device;
    }
    result.devices.clear();
    for (const auto& entry : unique) result.devices.push_back(entry.second);
    result.attempted_count = attempted.load();
    result.elapsed_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(ScanClock::now() - started).count());
    result.timed_out = completed.load() < plan.targets.size();
    result.message = result.timed_out ? "scan window ended; results may be partial" : "scan completed";
    if (result.timed_out) result.warnings.push_back("some targets did not complete before the scan deadline; narrow the range or increase timeout_ms");
    return result;
}

std::vector<DiscoveredDevice> ws_discovery_probe(int timeout_ms) {
    DiscoveryOptions options; options.timeout_ms = timeout_ms;
    return discover_cameras(options).devices;
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
