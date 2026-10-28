#include <limits>
#include "config_compiler.h"
#include "test_util.h"
using namespace aibox;
using namespace aibox::biz;
namespace {
void run() {
    CamAlgoRow on;
    on.algo_id = "person";
    on.roi_json = "[[0,0],[1,0],[1,1]]";
    on.rule_json = R"({"rule_type":"dwell","dwell_sec":12,"debounce_sec":7})";
    CamAlgoRow off;
    off.algo_id = "disabled";
    off.enabled = false;
    proto::SetAlgos out;
    std::string err;
    CHECK(compile_algos(3, {on, off}, out, err));
    CHECK(out.id == 3 && out.algos.size() == 1);
    if (!out.algos.empty()) {
        CHECK(out.algos[0].has_roi);
        CHECK(out.algos[0].dwell_sec == 12 && out.algos[0].debounce_sec == 7);
    }
    CHECK(compile_algos(3, {off}, out, err) && out.algos.empty());
    CHECK(!compile_algos(3, {on, on}, out, err) && out.algos.empty());
    for (const auto& roi : {"null", "[]", "[[0,0],[1,1]]", "[[0,0],[1,0],[2,1]]",
                            "[[0,0],[0,0],[0,0]]", "[[0,0],[1,0],[\"x\",1]]"}) {
        auto bad = on;
        bad.roi_json = roi;
        CHECK(!compile_algos(3, {bad}, out, err) && out.algos.empty());
    }
    auto bad = on;
    bad.threshold = std::numeric_limits<double>::quiet_NaN();
    CHECK(!compile_algos(3, {bad}, out, err));
    bad = on;
    bad.rule_json = R"({"rule_type":"unsupported"})";
    CHECK(!compile_algos(3, {bad}, out, err));
    bad.rule_json = R"({"dwell_sec":-1})";
    CHECK(!compile_algos(3, {bad}, out, err));
    bad = on;
    bad.detect_fps = 256;
    CHECK(!compile_algos(3, {bad}, out, err));
}
}
TEST_MAIN("config_compiler")
