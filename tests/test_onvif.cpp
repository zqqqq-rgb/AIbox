// onvif_client 纯函数单测（M5 摄像头扫描添加的支撑层）：
//   ① UsernameToken PasswordDigest（与 node crypto 独立实现对照）
//   ② xml_first_text：带/无命名空间前缀、实体解码、自闭合、缺失
//   ③ xml_tag_attrs：多 Profile token 按序去重
//   ④ rtsp_insert_credentials：插入/替换/特殊字符转义
// 网络相关（ws_discovery_probe / onvif_get_stream_uris）属集成行为，冒烟覆盖。
#include <string>
#include <vector>

#include "onvif_client.h"
#include "test_util.h"

using namespace aibox::biz;

namespace {

// 字符串/大小相等断言（test_util.h 未提供）
#define CHECK_EQ(a, b) CHECK_MSG((a) == (b), "got [%s] want [%s]", \
                                 std::string(a).c_str(), std::string(b).c_str())

void test_digest() {
    // 期望值由 node:crypto sha1(nonce+created+pass).base64 独立生成
    CHECK_EQ(wsu_password_digest("12345678", "2026-01-01T00:00:00Z", "admin"),
             std::string("nOcwyHR0Hh10zpiqb9fcURr7dbM="));
    // 空输入不崩（base64 输出恒 28 字节）
    CHECK(wsu_password_digest("", "", "").size() == 28);
}

void test_xml_first_text() {
    // 带前缀
    CHECK_EQ(xml_first_text("<a:Envelope><tt:Uri>rtsp://1.2.3.4:554/x</tt:Uri></a:Envelope>",
                            "Uri"),
             std::string("rtsp://1.2.3.4:554/x"));
    // 无前缀 + 前缀标签名同存时精确匹配（Uri 不误命中 UriPath）
    CHECK_EQ(xml_first_text("<r><Uri>ok</Uri><UriPath>bad</UriPath></r>", "Uri"),
             std::string("ok"));
    // 实体解码（大华子码流 URI 带 &）
    CHECK_EQ(xml_first_text("<tt:Uri>rtsp://ip/x?c=1&amp;s=1</tt:Uri>", "Uri"),
             std::string("rtsp://ip/x?c=1&s=1"));
    // 自闭合 / 缺失 → 空
    CHECK(xml_first_text("<XAddrs/>", "XAddrs").empty());
    CHECK(xml_first_text("<r><Other>x</Other></r>", "XAddrs").empty());
    // 带属性的标签
    CHECK_EQ(xml_first_text("<tt:MediaUri><tt:Uri>u9</tt:Uri></tt:MediaUri>", "Uri"),
             std::string("u9"));
}

void test_xml_tag_attrs() {
    const std::string resp =
        "<trt:GetProfilesResponse>"
        "<trt:Profile token=\"profile_1\"><Name>main</Name></trt:Profile>"
        "<trt:Profile token=\"profile_2\"><Name>sub</Name></trt:Profile>"
        "<trt:Profile token=\"profile_1\"/><trt:Other token=\"x\"/>"
        "</trt:GetProfilesResponse>";
    const std::vector<std::string> v = xml_tag_attrs(resp, "Profile", "token");
    CHECK(v.size() == 2);  // 全局去重：profile_1 重复与 Other 不入
    if (v.size() == 2) {
        CHECK_EQ(v[0], std::string("profile_1"));
        CHECK_EQ(v[1], std::string("profile_2"));
    }
    CHECK(xml_tag_attrs("<a/>", "Profile", "token").empty());
}

void test_rtsp_insert_credentials() {
    // 无账密 → 插入
    CHECK_EQ(rtsp_insert_credentials("rtsp://192.168.1.64:554/Streaming/Channels/101",
                                     "admin", "Abc@123"),
             std::string("rtsp://admin:Abc%40123@192.168.1.64:554/Streaming/Channels/101"));
    // 已有账密 → 替换
    CHECK_EQ(rtsp_insert_credentials("rtsp://old:old@192.168.1.64/x", "u", "p"),
             std::string("rtsp://u:p@192.168.1.64/x"));
    // 密码含保留字符全转义
    CHECK_EQ(rtsp_insert_credentials("rtsp://10.0.0.5/cam", "u", ":/#?&="),
             std::string("rtsp://u:%3A%2F%23%3F%26%3D@10.0.0.5/cam"));
    // 非法 URI 原样返回；用户名为空原样返回
    CHECK_EQ(rtsp_insert_credentials("not-a-uri", "u", "p"), std::string("not-a-uri"));
    CHECK_EQ(rtsp_insert_credentials("rtsp://ip/x", "", "p"), std::string("rtsp://ip/x"));
}

}  // namespace

void run() {
    test_digest();
    test_xml_first_text();
    test_xml_tag_attrs();
    test_rtsp_insert_credentials();
}

TEST_MAIN("onvif")
