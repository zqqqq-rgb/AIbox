#include "zlm_client.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "common/Log.h"

namespace aibox {
namespace biz {

namespace {

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(hex[u >> 4]);
            out.push_back(hex[u & 0xF]);
        }
    }
    return out;
}

std::string encode_form(const std::vector<std::pair<std::string, std::string>>& form) {
    std::string out;
    for (const auto& kv : form) {
        if (!out.empty()) out.push_back('&');
        out += url_encode(kv.first) + "=" + url_encode(kv.second);
    }
    return out;
}

// 带超时的 TCP 连接（host 支持点分 IPv4 与域名；ZLM 部署在 127.0.0.1）
int connect_tcp(const std::string& host, int port, int timeout_ms) {
    char port_buf[16];
    std::snprintf(port_buf, sizeof(port_buf), "%d", port);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), port_buf, &hints, &res) != 0 || !res) return -1;

    const int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        ::freeaddrinfo(res);
        return -1;
    }
    const int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    const int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    ::freeaddrinfo(res);
    if (rc != 0 && errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }
    if (rc != 0) {
        pollfd pf{};
        pf.fd = fd;
        pf.events = POLLOUT;
        if (::poll(&pf, 1, timeout_ms) != 1) {
            ::close(fd);
            return -1;
        }
        int so_err = 0;
        socklen_t len = sizeof(so_err);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &len);
        if (so_err != 0) {
            ::close(fd);
            return -1;
        }
    }
    return fd;
}

bool send_all(int fd, const std::string& buf, int timeout_ms) {
    size_t off = 0;
    while (off < buf.size()) {
        pollfd pf{};
        pf.fd = fd;
        pf.events = POLLOUT;
        if (::poll(&pf, 1, timeout_ms) != 1) return false;
        const ssize_t n = ::send(fd, buf.data() + off, buf.size() - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

// Connection: close：读到 EOF 即收全；poll 超时兜底
bool recv_all(int fd, std::string* out, int timeout_ms) {
    char buf[4096];
    for (;;) {
        pollfd pf{};
        pf.fd = fd;
        pf.events = POLLIN;
        const int pr = ::poll(&pf, 1, timeout_ms);
        if (pr == 0) return false;  // 应答超时
        if (pr < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            out->append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) return true;  // 对端关完
        if (errno == EINTR) continue;
        return false;
    }
}

// 解析 HTTP/1.1 应答：status 行 + Content-Length 定界（缺失则 EOF 定界）
bool parse_http_response(const std::string& raw, int* status, std::string* body) {
    const size_t hdr_end = raw.find("\r\n\r\n");
    if (hdr_end == std::string::npos) return false;
    if (raw.compare(0, 5, "HTTP/") != 0) return false;
    const size_t sp1 = raw.find(' ');
    if (sp1 == std::string::npos) return false;
    *status = std::atoi(raw.c_str() + sp1 + 1);

    size_t content_len = std::string::npos;
    size_t pos = 0;
    while (pos < hdr_end) {
        const size_t line_end = raw.find("\r\n", pos);
        const size_t stop = (line_end == std::string::npos || line_end > hdr_end)
                                ? hdr_end
                                : line_end;
        const std::string line = raw.substr(pos, stop - pos);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            for (char& c : name) c = static_cast<char>(::tolower(c));
            if (name == "content-length") {
                content_len = static_cast<size_t>(std::atoll(line.c_str() + colon + 1));
                break;
            }
        }
        if (line_end == std::string::npos || line_end >= hdr_end) break;
        pos = line_end + 2;
    }
    *body = raw.substr(hdr_end + 4, content_len == std::string::npos
                                          ? std::string::npos
                                          : content_len);
    return true;
}

}  // namespace

ZlmClient::ZlmClient(Params p) : p_(std::move(p)) {}

// ---- HTTP / API 原语（无锁内部版；公共方法持 mu_）----

bool ZlmClient::http(const char* method, const std::string& path,
                     const std::string& encoded_form, std::string* body_out,
                     int timeout_ms) {
    const int tmo = timeout_ms > 0 ? timeout_ms : p_.timeout_ms;
    const int fd = connect_tcp(p_.host, p_.port, tmo);
    if (fd < 0) return false;

    std::string req = std::string(method) + " " + path + " HTTP/1.1\r\n";
    req += "Host: " + p_.host + ":" + std::to_string(p_.port) + "\r\n";
    req += "Accept: application/json\r\n";
    if (!encoded_form.empty()) {
        req += "Content-Type: application/x-www-form-urlencoded\r\n";
        req += "Content-Length: " + std::to_string(encoded_form.size()) + "\r\n";
    }
    req += "Connection: close\r\n\r\n";
    req += encoded_form;

    std::string raw;
    const bool ok = send_all(fd, req, tmo) && recv_all(fd, &raw, tmo);
    ::close(fd);
    if (!ok) return false;

    int status = 0;
    std::string body;
    if (!parse_http_response(raw, &status, &body)) return false;
    if (status != 200) return false;
    *body_out = std::move(body);
    return true;
}

bool ZlmClient::call_api(const char* method, const std::string& path,
                         const std::vector<std::pair<std::string, std::string>>& form,
                         int* code, std::string* msg, Json::Value* data) {
    *code = kErrTransport;
    std::vector<std::pair<std::string, std::string>> f = form;
    f.emplace_back("secret", p_.secret);
    // GET 参数走 query string（ZLM 对 GET body 不解析，secret 会被丢）；POST 走 form body
    const std::string encoded = encode_form(f);
    const bool is_get = std::strcmp(method, "GET") == 0;
    const std::string req_path = is_get ? path + "?" + encoded : path;
    const std::string req_body = is_get ? "" : encoded;
    std::string body;
    if (!http(method, req_path, req_body, &body)) {
        *msg = "zlm http transport failed";
        LOGW("zlm", "%s %s 传输失败", method, path.c_str());
        return false;
    }
    Json::Value root;
    Json::CharReaderBuilder b;
    std::unique_ptr<Json::CharReader> rd(b.newCharReader());
    std::string errs;
    if (!rd->parse(body.data(), body.data() + body.size(), &root, &errs)) {
        *msg = "zlm bad json: " + errs;
        LOGW("zlm", "%s %s 应答非 JSON: %.80s", method, path.c_str(), body.c_str());
        return false;
    }
    *code = root.get("code", kErrTransport).asInt();
    if (root.isMember("msg")) *msg = root["msg"].asString();
    if (root.isMember("data")) *data = root["data"];
    if (*code != kOk) {
        LOGW("zlm", "%s %s code=%d msg=%s", method, path.c_str(), *code, msg->c_str());
        return false;
    }
    return true;
}

bool ZlmClient::media_list_impl(Json::Value* out) {
    int code = kOk;
    std::string msg;
    return call_api("GET", "/index/api/getMediaList", {}, &code, &msg, out);
}

bool ZlmClient::find_proxy_key_impl(const std::string& app, const std::string& stream,
                                    std::string* key) {
    Json::Value list;
    if (!media_list_impl(&list) || !list.isArray()) return false;
    for (const auto& item : list) {
        if (item.isMember("app") && item.isMember("stream") &&
            item["app"].asString() == app && item["stream"].asString() == stream) {
            // key 按 ZLM 约定拼装：__defaultVhost__/app/stream（addStreamProxy
            // 应答的 data.key 与之一致）
            *key = "__defaultVhost__/" + app + "/" + stream;
            return true;
        }
    }
    return false;
}

ZlmClient::ProxyResult ZlmClient::ensure_proxy_impl(int cam, bool main,
                                                    const std::string& rtsp) {
    ProxyResult r;
    const std::string stream = "cam" + std::to_string(cam) + (main ? "_main" : "_sub");
    // 幂等：已存在直接返回既有 key（同名重添不产生双代理，详设 §4.1）
    std::string exist;
    if (find_proxy_key_impl("live", stream, &exist)) {
        r.key = exist;
        return r;
    }
    int code = kOk;
    std::string msg;
    Json::Value data;
    const std::vector<std::pair<std::string, std::string>> form = {
        {"vhost", "__defaultVhost__"}, {"app", "live"},       {"stream", stream},
        {"url", rtsp},                 {"rtp_type", "0"},     {"enable_mp4", main ? "1" : "0"},
        {"enable_hls", "0"},           {"retry_count", "-1"},
    };
    if (!call_api("POST", "/index/api/addStreamProxy", form, &code, &msg, &data)) {
        r.err = code == kOk ? kErrTransport : code;
        r.msg = msg;
        return r;
    }
    // ZLM: data.key = "__defaultVhost__/live/cam1_main"；
    // data 形态防御（板上 ZLM 版本差异实证）：非对象/缺 key 一律按约定拼装
    r.key = (data.isObject() && data.isMember("key") && data["key"].isString())
                ? data["key"].asString()
                : "__defaultVhost__/live/" + stream;
    r.created = true;
    LOGI("zlm", "addStreamProxy %s -> %s", stream.c_str(), r.key.c_str());
    return r;
}

bool ZlmClient::del_proxy_impl(const std::string& key) {
    int code = kOk;
    std::string msg;
    Json::Value data;
    const std::vector<std::pair<std::string, std::string>> form = {{"key", key}};
    if (!call_api("POST", "/index/api/delStreamProxy", form, &code, &msg, &data)) {
        LOGW("zlm", "delStreamProxy %s 失败（best-effort 继续）: %s", key.c_str(),
             msg.c_str());
        return false;
    }
    LOGI("zlm", "delStreamProxy %s", key.c_str());
    return true;
}

// ---- 公共方法（mu_ 薄锁壳）----

bool ZlmClient::media_list(Json::Value* out) {
    std::lock_guard<std::mutex> lk(mu_);
    return media_list_impl(out);
}

bool ZlmClient::get_server_config(Json::Value* out) {
    std::lock_guard<std::mutex> lk(mu_);
    int code = kOk;
    std::string msg;
    return call_api("GET", "/index/api/getServerConfig", {}, &code, &msg, out);
}

bool ZlmClient::get_snap(const std::string& url, int timeout_sec, std::string* jpeg) {
    std::lock_guard<std::mutex> lk(mu_);
    // getSnap 应答二态：成功 = image/jpeg 二进制；失败 = HTTP 200 + JSON 错误体。
    // 不走 call_api（它按 JSON 解析），直接 http 取原始体 + JPEG 魔数判定
    const std::string q =
        "url=" + url_encode(url) + "&timeout_sec=" + std::to_string(timeout_sec) +
        "&expire_sec=1&secret=" + url_encode(p_.secret);
    std::string body;
    // HTTP 层超时 = getSnap 等待 + 2s（传输/解析余量）
    if (!http("GET", "/index/api/getSnap?" + q, "", &body, timeout_sec * 1000 + 2000))
        return false;
    if (body.size() < 4 || body[0] != '\xFF' || body[1] != '\xD8')
        return false;  // 非JPEG（JSON 错误应答/截断）
    *jpeg = std::move(body);
    return true;
}

ZlmClient::ProxyResult ZlmClient::ensure_proxy(int cam, bool main,
                                               const std::string& rtsp) {
    std::lock_guard<std::mutex> lk(mu_);
    return ensure_proxy_impl(cam, main, rtsp);
}

bool ZlmClient::del_proxy(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    return del_proxy_impl(key);
}

bool ZlmClient::add_camera_proxies(int cam, const std::string& rtsp_main,
                                   const std::string& rtsp_sub, std::string* key_main,
                                   std::string* key_sub) {
    std::lock_guard<std::mutex> lk(mu_);  // 编排事务全程持锁（单写者 + 原子观察）
    const ProxyResult m = ensure_proxy_impl(cam, true, rtsp_main);
    if (m.err != kOk) {
        LOGE("zlm", "cam%d add main 失败: %s", cam, m.msg.c_str());
        return false;
    }
    const ProxyResult s = ensure_proxy_impl(cam, false, rtsp_sub);
    if (s.err != kOk) {
        // 详设 §5.3：任一失败 → 回滚已建代理（防半配置）
        LOGE("zlm", "cam%d add sub 失败（回滚 main 代理）: %s", cam, s.msg.c_str());
        if (m.created) del_proxy_impl(m.key);
        return false;
    }
    if (key_main) *key_main = m.key;
    if (key_sub) *key_sub = s.key;
    LOGI("zlm", "cam%d 代理编排完成: main=%s sub=%s", cam, m.key.c_str(), s.key.c_str());
    return true;
}

}  // namespace biz
}  // namespace aibox
