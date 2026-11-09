#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace aibox { namespace biz {

struct RemoteAccessConfig {
    std::string public_base_url;
    std::string public_ws_url;
    std::string public_media_signaling_path = "/media/index/api/webrtc";
};

// Validates and canonicalizes explicit external origins. Forwarded headers never
// select the origin: only the operator's configuration does.
bool validate_remote_access(RemoteAccessConfig& config, const std::string& token,
                            std::string& error);

class AccessSession {
public:
    using Clock = std::function<int64_t()>;
    struct Params {
        std::string token;
        std::string public_origin;
        int http_port = 80;
        int64_t ttl_ms = 30 * 60 * 1000;
        size_t max_sessions = 256;
        Clock clock;
    };
    explicit AccessSession(Params params);
    bool required() const { return !params_.token.empty(); }
    bool bearer_valid(const std::string& authorization) const;
    bool authenticated(const std::string& authorization, const std::string& cookie) const;
    bool valid_session(const std::string& id) const;
    // Empty Origin is allowed only when explicitly requested by a non-cookie
    // login/client flow; mutating cookie requests and browser WS require it.
    bool origin_allowed(const std::string& origin, const std::string& host,
                        bool allow_missing = false) const;
    std::string login(const std::string& token);
    void logout(const std::string& cookie);
    std::string cookie_header(const std::string& id) const;
    std::string clear_cookie_header() const;
    static std::string cookie_session(const std::string& cookie);

private:
    int64_t now() const;
    Params params_;
    mutable std::mutex mu_;
    mutable std::map<std::string, int64_t> sessions_;
};

} } // namespace aibox::biz
