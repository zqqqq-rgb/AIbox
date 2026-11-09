#include "access_session.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>

namespace aibox { namespace biz {
namespace {
bool secure_equal(const std::string& a, const std::string& b) {
    size_t difference = a.size() ^ b.size();
    const size_t length = std::max(a.size(), b.size());
    for (size_t i = 0; i < length; ++i)
        difference |= static_cast<unsigned char>(i < a.size() ? a[i] : 0) ^
                      static_cast<unsigned char>(i < b.size() ? b[i] : 0);
    return difference == 0;
}
bool origin(std::string& value, const char* scheme, std::string* host_out = nullptr) {
    const std::string prefix = std::string(scheme) + "://";
    if (value.rfind(prefix, 0) != 0 || value.size() > 512) return false;
    for (unsigned char c : value) if (c <= 32 || c >= 127) return false;
    if (value.back() == '/') value.pop_back();
    std::string authority = value.substr(prefix.size()), host, port;
    if (authority.empty() || authority.find_first_of("/?#@\\") != std::string::npos) return false;
    if (authority.front() == '[') {
        const size_t end = authority.find(']');
        if (end == std::string::npos) return false;
        host = authority.substr(0, end + 1);
        in6_addr address{};
        if (inet_pton(AF_INET6, host.substr(1, host.size() - 2).c_str(), &address) != 1) return false;
        if (end + 1 < authority.size()) {
            if (authority[end + 1] != ':') return false;
            port = authority.substr(end + 2);
            if (port.empty()) return false;
        }
    } else {
        const size_t colon = authority.find(':');
        host = authority.substr(0, colon);
        if (colon != std::string::npos) {
            port = authority.substr(colon + 1);
            if (port.empty()) return false;
        }
        if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.') return false;
        bool label_start = true;
        char previous = 0;
        for (unsigned char c : host) {
            if (!(std::isalnum(c) || c == '-' || c == '.')) return false;
            if ((label_start && c == '-') || (c == '.' && (label_start || previous == '-'))) return false;
            label_start = c == '.';
            previous = static_cast<char>(c);
        }
        if (previous == '-') return false;
    }
    if (!port.empty()) {
        unsigned number = 0;
        for (char c : port) {
            if (c < '0' || c > '9' || number > 6553) return false;
            number = number * 10 + (c - '0');
            if (number > 65535) return false;
        }
        if (number == 0) return false;
        if ((number == 443 && (std::string(scheme) == "https" || std::string(scheme) == "wss")) ||
            (number == 80 && std::string(scheme) == "http")) port.clear();
        else port = std::to_string(number);
    }
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
    value = prefix + host + (port.empty() ? "" : ":" + port);
    if (host_out) *host_out = host;
    return true;
}
std::string trim(std::string value) {
    const size_t first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t") - first + 1);
}
} // namespace

bool validate_remote_access(RemoteAccessConfig& config, const std::string& token,
                            std::string& error) {
    std::string host;
    if (!config.public_base_url.empty()) {
        if (!origin(config.public_base_url, "https", &host)) {
            error = "public_base_url must be an HTTPS origin without credentials, path, query or fragment"; return false;
        }
        if (token.empty()) { error = "public_base_url requires a non-empty auth_token"; return false; }
    }
    if (!config.public_ws_url.empty()) {
        std::string ws_host;
        if (host.empty() || !origin(config.public_ws_url, "wss", &ws_host) || host != ws_host) {
            error = "public_ws_url must be a WSS origin on the public_base_url host"; return false;
        }
    }
    const auto& path = config.public_media_signaling_path;
    if (path.empty() || path.size() > 256 || path.front() != '/' || path.rfind("//", 0) == 0 ||
        path.find_first_of("?#\\%") != std::string::npos || path.find("..") != std::string::npos) {
        error = "public_media_signaling_path must be a local absolute path without query or traversal"; return false;
    }
    for (unsigned char c : path) if (!(std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.')) {
        error = "public_media_signaling_path contains unsupported characters"; return false;
    }
    return true;
}

AccessSession::AccessSession(Params params) : params_(std::move(params)) {
    if (params_.ttl_ms <= 0) params_.ttl_ms = 30 * 60 * 1000;
}
int64_t AccessSession::now() const {
    if (params_.clock) return params_.clock();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool AccessSession::bearer_valid(const std::string& authorization) const {
    return required() && secure_equal(authorization, "Bearer " + params_.token);
}
std::string AccessSession::cookie_session(const std::string& cookie) {
    std::string result;
    size_t begin = 0;
    bool found = false;
    while (begin < cookie.size()) {
        const auto end = cookie.find(';', begin);
        const auto part = trim(cookie.substr(begin, end == std::string::npos ? end : end - begin));
        if (part.rfind("aibox_session=", 0) == 0) {
            if (found) return {};
            found = true; result = part.substr(14);
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    if (result.size() != 64 || result.find_first_not_of("0123456789abcdef") != std::string::npos) return {};
    return result;
}
bool AccessSession::valid_session(const std::string& id) const {
    if (id.empty()) return false;
    std::lock_guard<std::mutex> lock(mu_);
    auto found = sessions_.find(id);
    if (found == sessions_.end()) return false;
    if (found->second <= now()) { sessions_.erase(found); return false; }
    return true;
}
bool AccessSession::authenticated(const std::string& authorization, const std::string& cookie) const {
    return !required() || bearer_valid(authorization) || valid_session(cookie_session(cookie));
}
bool AccessSession::origin_allowed(const std::string& supplied, const std::string& host, bool allow_missing) const {
    if (supplied.empty()) return allow_missing;
    std::string candidate = supplied;
    const char* scheme = params_.public_origin.empty() ? "http" : "https";
    if (!origin(candidate, scheme) || supplied.find_first_of("?#") != std::string::npos) return false;
    if (!params_.public_origin.empty()) return candidate == params_.public_origin;
    // WS runs on a separate port locally. The page Origin uses the HTTP port,
    // while the handshake Host uses the WS port, so compare its host separately.
    std::string authority = host;
    if (authority.empty()) return false;
    if (authority.front() == '[') {
        const auto end = authority.find(']');
        if (end == std::string::npos) return false;
        authority = authority.substr(0, end + 1);
    } else {
        const auto colon = authority.find(':');
        authority = authority.substr(0, colon);
    }
    std::string expected = "http://" + authority + (params_.http_port == 80 ? "" : ":" + std::to_string(params_.http_port));
    return origin(expected, "http") && candidate == expected;
}
std::string AccessSession::login(const std::string& token) {
    if (!required() || !secure_equal(token, params_.token)) return {};
    unsigned char bytes[32];
    const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    size_t used = 0;
    while (used < sizeof(bytes)) {
        const ssize_t count = read(fd, bytes + used, sizeof(bytes) - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { close(fd); return {}; }
        used += static_cast<size_t>(count);
    }
    close(fd);
    static const char* hex = "0123456789abcdef";
    std::string id; id.reserve(64);
    for (unsigned char byte : bytes) { id += hex[byte >> 4]; id += hex[byte & 15]; }
    std::lock_guard<std::mutex> lock(mu_);
    const int64_t timestamp = now();
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->second <= timestamp) it = sessions_.erase(it); else ++it;
    }
    if (sessions_.size() >= params_.max_sessions) return {};
    sessions_[id] = timestamp + params_.ttl_ms;
    return id;
}
void AccessSession::logout(const std::string& cookie) {
    std::lock_guard<std::mutex> lock(mu_);
    sessions_.erase(cookie_session(cookie));
}
std::string AccessSession::cookie_header(const std::string& id) const {
    return "aibox_session=" + id + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" +
           std::to_string(params_.ttl_ms / 1000) + (params_.public_origin.empty() ? "" : "; Secure");
}
std::string AccessSession::clear_cookie_header() const {
    return "aibox_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0" +
           std::string(params_.public_origin.empty() ? "" : "; Secure");
}
} } // namespace aibox::biz
