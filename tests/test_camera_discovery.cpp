#include "onvif_client.h"
#include "test_util.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace aibox::biz;
namespace {
const DiscoveryInterface lan{"eth0", "192.168.10.17", "255.255.255.0", ""};
bool has(const DiscoveryPlan& plan, const char* ip) {
    return std::find(plan.targets.begin(), plan.targets.end(), ip) != plan.targets.end();
}
void test_default_scope() {
    DiscoveryPlan plan; std::string error;
    CHECK(plan_discovery_targets({lan}, {}, plan, error));
    CHECK(plan.targets.size() == 253);
    CHECK(!has(plan, "192.168.10.17"));
    CHECK(!has(plan, "192.168.10.0"));
    CHECK(!has(plan, "192.168.10.255"));
    CHECK(has(plan, "192.168.10.1") && has(plan, "192.168.10.254"));
    CHECK(plan.cidrs == std::vector<std::string>{"192.168.10.0/24"});
    CHECK(plan.interfaces[0].cidr == "192.168.10.0/24");
    const DiscoveryInterface large{"eth0", "10.22.3.6", "255.255.0.0", ""};
    CHECK(plan_discovery_targets({large}, {}, plan, error));
    CHECK(plan.targets.size() == 253);
    CHECK(plan.cidrs[0] == "10.22.3.0/24");
    CHECK(plan.interfaces[0].cidr == "10.22.0.0/16");
    const DiscoveryInterface small{"eth0", "10.22.3.6", "255.255.255.252", ""};
    CHECK(plan_discovery_targets({small}, {}, plan, error));
    CHECK(plan.targets == std::vector<std::string>{"10.22.3.5"});
}
void test_explicit_scope_and_selection() {
    DiscoveryPlan plan; std::string error;
    DiscoveryOptions options; options.interface_name = "eth0"; options.cidr = "192.168.10.65/26";
    CHECK(plan_discovery_targets({lan, {"eth1", "10.0.0.1", "255.255.255.0", ""}}, options, plan, error));
    CHECK(plan.cidrs == std::vector<std::string>{"192.168.10.64/26"});
    CHECK(plan.targets.size() == 62);
    CHECK(has(plan, "192.168.10.65") && has(plan, "192.168.10.126"));
    CHECK(!has(plan, "192.168.10.64") && !has(plan, "192.168.10.127"));
    CHECK(plan.interfaces.size() == 1 && plan.interfaces[0].name == "eth0");
    options.cidr = "192.168.11.0/24";
    CHECK(!plan_discovery_targets({lan}, options, plan, error));
    CHECK(plan.targets.empty() && plan.interfaces.empty());
    options.cidr = "192.168.10.0/23";
    CHECK(!plan_discovery_targets({lan}, options, plan, error));
    options.interface_name.clear(); options.cidr = "192.168.10.0/24";
    CHECK(!plan_discovery_targets({lan}, options, plan, error));
    options.interface_name = "not-an-interface"; options.cidr.clear();
    CHECK(!plan_discovery_targets({lan}, options, plan, error));
    CHECK(!plan_discovery_targets({}, {}, plan, error));
}
void test_aliases_dedup_and_limits() {
    DiscoveryPlan plan; std::string error;
    const DiscoveryInterface alias{"eth0", "192.168.10.18", "255.255.255.0", ""};
    CHECK(plan_discovery_targets({lan, alias, lan}, {}, plan, error));
    CHECK(plan.targets.size() == 252 && plan.interfaces.size() == 2 && plan.cidrs.size() == 1);
    CHECK(!has(plan, "192.168.10.17") && !has(plan, "192.168.10.18"));
    DiscoveryOptions options; options.interface_name = "eth0"; options.cidr = "10.22.0.0/22";
    const DiscoveryInterface large{"eth0", "10.22.3.6", "255.255.0.0", ""};
    CHECK(plan_discovery_targets({large}, options, plan, error));
    CHECK(plan.targets.size() == 1021);
    options.cidr = "10.22.0.0/21";
    CHECK(!plan_discovery_targets({large}, options, plan, error));
    CHECK(plan.targets.empty());
    std::vector<DiscoveryInterface> many;
    for (int i = 0; i < 5; ++i) many.push_back({"eth" + std::to_string(i), "10.0." + std::to_string(i) + ".1", "255.255.255.0", ""});
    CHECK(!plan_discovery_targets(many, {}, plan, error));
    options = {}; options.interface_name = "eth3";
    CHECK(plan_discovery_targets(many, options, plan, error));
    CHECK(plan.targets.size() == 253);
    // A name may have more than one address; the chosen CIDR only needs to belong to one.
    options = {"eth0", "10.22.3.128/25", 3000};
    CHECK(plan_discovery_targets({lan, large}, options, plan, error));
    CHECK(plan.targets.size() == 126 && plan.interfaces.size() == 1);
}
void test_host_ranges_and_validation() {
    DiscoveryPlan plan; std::string error;
    DiscoveryOptions options{"eth0", "192.168.10.44/32", 500};
    CHECK(plan_discovery_targets({lan}, options, plan, error));
    CHECK(plan.targets == std::vector<std::string>{"192.168.10.44"});
    options.cidr = "192.168.10.17/32";
    CHECK(plan_discovery_targets({lan}, options, plan, error));
    CHECK(plan.targets.empty());
    options.cidr = "192.168.10.255/32";
    CHECK(plan_discovery_targets({lan}, options, plan, error));
    CHECK(plan.targets.empty());
    options.cidr = "192.168.10.44/31";
    CHECK(plan_discovery_targets({lan}, options, plan, error));
    CHECK(plan.targets.size() == 2);
    const DiscoveryInterface point{"eth0", "10.0.0.0", "255.255.255.254", ""};
    options.cidr.clear();
    CHECK(plan_discovery_targets({point}, options, plan, error));
    CHECK(plan.targets == std::vector<std::string>{"10.0.0.1"});
    for (const std::string cidr : {"bad", "1.2.3.4", "192.168.10.1/0", "192.168.10.1/33", "192.168.10.1/-1", "192.168.10.1/24x", "192.168.10.1/24/25", "192.168.10.256/24", "192.168.10.1/ 24"}) {
        options.cidr = cidr;
        CHECK(!plan_discovery_targets({lan}, options, plan, error));
    }
    options.cidr.clear(); options.timeout_ms = 499;
    CHECK(!plan_discovery_targets({lan}, options, plan, error));
    options.timeout_ms = 10001;
    CHECK(!plan_discovery_targets({lan}, options, plan, error));
    options.timeout_ms = 10000;
    CHECK(!plan_discovery_targets({{"eth0", "127.0.0.1", "255.255.255.0", ""}}, options, plan, error));
    CHECK(!plan_discovery_targets({{"eth0", "10.0.0.1", "255.0.255.0", ""}}, options, plan, error));
}
void test_response_classification() {
    bool confirmed = true;
    CHECK(!classify_onvif_probe_response("HTTP/1.1 200 OK\r\n\r\n<html>camera</html>", confirmed));
    CHECK(!confirmed);
    CHECK(classify_onvif_probe_response("HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n", confirmed));
    CHECK(!confirmed);
    CHECK(classify_onvif_probe_response("HTTP/1.1 500 Error\r\n\r\n<s:Fault><faultcode>Denied</faultcode></s:Fault>", confirmed));
    CHECK(!confirmed);
    CHECK(classify_onvif_probe_response("HTTP/1.1 200 OK\r\n\r\n<tds:GetSystemDateAndTimeResponse xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"/>", confirmed));
    CHECK(confirmed);
    CHECK(!classify_onvif_probe_response("RTSP/1.0 401 Unauthorized\r\n\r\n", confirmed));
    CHECK(!confirmed);
    CHECK(!classify_onvif_probe_response("HTTP/1.1 200 OK\r\n", confirmed));
}
}
void run() {
    test_default_scope(); test_explicit_scope_and_selection(); test_aliases_dedup_and_limits();
    test_host_ranges_and_validation(); test_response_classification();
}
TEST_MAIN("camera_discovery")
