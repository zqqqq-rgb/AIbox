// T4.3 单测（§4-T4.3 断言表）：ZLM HTTP mock（tests/mock_zlm.h，脚本化应答）
//   ① 编排事务成功：main+sub 均建 → 两 key 回填
//   ② 事务回滚：main 成功 + sub 失败 → del(main_key) 被调用（防半配置）
//   ③ 同名幂等：getMediaList 已存在的代理不重添（add 计数不增）
//   ④ 单代理 ensure/del 幂等
//   ⑤ 传输失败：连不上 ZLM → false（错误码 4002 语义）
#include <memory>

#include "mock_zlm.h"
#include "test_util.h"
#include "zlm_client.h"

using namespace aibox;
using namespace aibox::biz;
using aibox::testutil::MockZlm;

namespace {

// 每个用例独立 mock + client（ZlmClient 不可拷贝：unique_ptr 持有）
struct Rig {
    MockZlm mock;
    std::unique_ptr<ZlmClient> client;

    Rig() {
        CHECK(mock.start());
        client = std::make_unique<ZlmClient>(
            ZlmClient::Params{"127.0.0.1", mock.port(), "test-secret", 2000});
    }
    ~Rig() { mock.stop(); }
};

void test_transaction_success() {
    Rig r;
    std::string km, ks;
    CHECK(r.client->add_camera_proxies(1, "rtsp://m1", "rtsp://s1", &km, &ks));
    CHECK(km == "__defaultVhost__/live/cam1_main");
    CHECK(ks == "__defaultVhost__/live/cam1_sub");
    CHECK(r.mock.add_main.load() == 1 && r.mock.add_sub.load() == 1);
    CHECK(r.mock.del.load() == 0);  // 成功无回滚
}

void test_transaction_rollback() {
    Rig r;
    r.mock.fail_sub_add = true;  // main 成功 + sub 失败 → 回滚 main（§4-T4.3 断言）
    std::string km, ks;
    CHECK(!r.client->add_camera_proxies(2, "rtsp://m2", "rtsp://s2", &km, &ks));
    CHECK(r.mock.add_main.load() == 1 && r.mock.add_sub.load() == 1);
    CHECK(r.mock.del.load() == 1);
    const auto deleted = r.mock.deleted_keys();
    CHECK(deleted.size() == 1);
    CHECK(deleted[0] == "__defaultVhost__/live/cam2_main");
}

void test_idempotent_same_name() {
    Rig r;
    r.mock.seed_proxy("live", "cam1_main");  // ZLM 已有同名代理（如上次运行残留）
    std::string km, ks;
    CHECK(r.client->add_camera_proxies(1, "rtsp://m1", "rtsp://s1", &km, &ks));
    CHECK(km == "__defaultVhost__/live/cam1_main");
    CHECK(r.mock.add_main.load() == 0);  // 已存在 → 不重添
    CHECK(r.mock.add_sub.load() == 1);   // sub 正常创建
}

void test_single_proxy_and_del() {
    Rig r;
    const auto res = r.client->ensure_proxy(3, true, "rtsp://m3");
    CHECK(res.err == ZlmClient::kOk);
    CHECK(res.created);
    CHECK(res.key == "__defaultVhost__/live/cam3_main");
    // 幂等：再次 ensure 不重添
    const auto res2 = r.client->ensure_proxy(3, true, "rtsp://m3");
    CHECK(res2.key == res.key);
    CHECK(!res2.created);
    CHECK(r.mock.add_main.load() == 1);
    CHECK(r.client->del_proxy(res.key));
    CHECK(r.mock.del.load() == 1);
    CHECK(r.mock.deleted_keys()[0] == res.key);
}

void test_transport_failure() {
    MockZlm mock;  // 不 start：端口未监听
    ZlmClient client(ZlmClient::Params{"127.0.0.1", 1 /*保留端口，必拒*/, "s", 200});
    std::string km, ks;
    CHECK(!client.add_camera_proxies(9, "rtsp://m", "rtsp://s", &km, &ks));
    Json::Value list;
    CHECK(!client.media_list(&list));  // 传输失败 → false（错误码 4002 语义）
}

void test_rollback_preserves_existing_main() {
    Rig r;
    r.mock.seed_proxy("live", "cam2_main");
    r.mock.fail_sub_add = true;
    std::string km, ks;
    CHECK(!r.client->add_camera_proxies(2, "rtsp://m2", "rtsp://s2", &km, &ks));
    CHECK(r.mock.del.load() == 0);
    CHECK(r.mock.deleted_keys().empty());
    const auto main = r.client->ensure_proxy(2, true, "rtsp://m2");
    CHECK(main.err == ZlmClient::kOk && !main.created);
    CHECK(r.mock.add_main.load() == 0);
}

void run() {
    test_transaction_success();
    test_transaction_rollback();
    test_idempotent_same_name();
    test_single_proxy_and_del();
    test_transport_failure();
    test_rollback_preserves_existing_main();
}

}  // namespace

TEST_MAIN("zlm_client")
