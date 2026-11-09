#include "access_session.h"
#include "test_util.h"

#include <atomic>

using namespace aibox::biz;
namespace {
void run() {
    std::string error;
    RemoteAccessConfig config;
    CHECK(validate_remote_access(config, "", error));
    config.public_base_url = "https://AIBOX.example.com:443/";
    CHECK(!validate_remote_access(config, "", error));
    CHECK(validate_remote_access(config, "credential", error));
    CHECK(config.public_base_url == "https://aibox.example.com");
    config.public_ws_url = "wss://aibox.example.com:9443";
    CHECK(validate_remote_access(config, "credential", error));
    for (const char* bad : {"http://aibox.example.com", "https://u:p@aibox.example.com", "https://aibox.example.com/path",
                            "https://aibox.example.com?x=1", "https://aibox.example.com#frag", "https://aibox.example.com\r\nX: injected",
                            "https://aibox.example.com:0", "https://aibox.example.com:65536", "https://aibox.example.com:",
                            "https://aibox..example.com", "https://-aibox.example.com"}) {
        auto bad_config = config; bad_config.public_base_url = bad;
        CHECK(!validate_remote_access(bad_config, "credential", error));
    }
    for (const char* bad : {"ws://aibox.example.com", "wss://other.example.com", "wss://aibox.example.com/ws"}) {
        auto bad_config = config; bad_config.public_ws_url = bad;
        CHECK(!validate_remote_access(bad_config, "credential", error));
    }
    for (const char* bad : {"//evil/path", "/path?secret=x", "/../private", "/%2e%2e/private", "/path\r\nX: evil"}) {
        auto bad_config = config; bad_config.public_media_signaling_path = bad;
        CHECK(!validate_remote_access(bad_config, "credential", error));
    }
    config.public_base_url = "https://[::1]:443"; config.public_ws_url = "wss://[::1]";
    CHECK(validate_remote_access(config, "credential", error));

    std::atomic<int64_t> now{100};
    AccessSession::Params params;
    params.token = "test-configured-credential"; params.public_origin = "https://aibox.example.com";
    params.ttl_ms = 1000; params.max_sessions = 2; params.clock = [&] { return now.load(); };
    AccessSession session(params);
    CHECK(session.required());
    CHECK(!session.authenticated("", ""));
    CHECK(session.bearer_valid("Bearer test-configured-credential"));
    CHECK(!session.bearer_valid("Bearer test-configured-credential-extra"));
    CHECK(session.login("bad").empty());
    const auto id = session.login(params.token), second = session.login(params.token);
    CHECK(id.size() == 64 && second.size() == 64 && id != second);
    CHECK(session.login(params.token).empty());
    CHECK(session.cookie_header(id).find("HttpOnly; SameSite=Strict") != std::string::npos);
    CHECK(session.cookie_header(id).find("; Secure") != std::string::npos);
    const auto cookie = "other=ignored; aibox_session=" + id;
    CHECK(session.authenticated("", cookie));
    CHECK(AccessSession::cookie_session(cookie) == id);
    CHECK(AccessSession::cookie_session(cookie + "; aibox_session=" + second).empty());
    CHECK(!session.origin_allowed("https://evil.example.com", "aibox.example.com"));
    CHECK(!session.origin_allowed("", "aibox.example.com"));
    CHECK(session.origin_allowed("", "aibox.example.com", true));
    CHECK(session.origin_allowed("https://aibox.example.com", "untrusted-proxy-host"));
    session.logout(cookie);
    CHECK(!session.valid_session(id));
    CHECK(session.valid_session(second));
    now = 1100;
    CHECK(!session.valid_session(second));
    CHECK(!session.login(params.token).empty());

    params.public_origin.clear(); params.http_port = 8080;
    AccessSession local(params);
    CHECK(local.origin_allowed("http://192.168.1.2:8080", "192.168.1.2:8081"));
    CHECK(!local.origin_allowed("http://192.168.1.2:8081", "192.168.1.2:8081"));
    CHECK(!local.origin_allowed("http://evil.example.com:8080", "192.168.1.2:8081"));
    CHECK(local.cookie_header("id").find("Secure") == std::string::npos);
    params.token.clear(); AccessSession open(params);
    CHECK(!open.required() && open.authenticated("", ""));
}
}
TEST_MAIN("access_session")
