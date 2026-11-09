#include "db.h"
#include "infer/NpuScheduler.h"
#include "rules/RuleRuntime.h"
#include "task_compiler.h"
#include "test_util.h"
#include <future>
#include <limits>
#include <sstream>
using namespace aibox;
using namespace aibox::biz;
namespace {
proto::RuleCfg rule(uint64_t id, float threshold = .5f, uint8_t fps = 10) {
    proto::RuleCfg r;
    r.task_id = id;
    r.rule_id = id + 100;
    r.task_revision = 2;
    r.rule_version = 3;
    r.event_type = "same_event";
    r.threshold = threshold;
    r.fps = fps;
    r.roi = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    r.debounce_sec = 30;
    return r;
}
proto::SetAlgos config(std::vector<proto::RuleCfg> rules) {
    proto::SetAlgos c;
    c.id = 1;
    c.config_version = 2;
    proto::AlgoCfg a;
    a.algo = "person";
    a.has_rules = true;
    a.rules = std::move(rules);
    a.thr = .1f;
    a.fps = 10;
    c.algos.push_back(a);
    return c;
}
RuleRuntime::Result evaluate(RuleRuntime& r, uint64_t g, int64_t ms, float confidence = .8f,
                             const std::string& model = "person") {
    proto::DetTarget target;
    target.conf = confidence;
    target.x = 20;
    target.y = 20;
    target.w = 20;
    target.h = 20;
    std::vector<proto::DetTarget> targets{target};
    return r.evaluate(1, model, g, targets, ms, 100, 100);
}
Json::Value json(const std::string& text) {
    Json::Value value;
    std::istringstream(text) >> value;
    return value;
}
std::string dump(const Json::Value& value) {
    Json::StreamWriterBuilder b;
    return Json::writeString(b, value);
}
void protocol() {
    auto c = config({rule(1), rule(2)});
    proto::ParsedMsg parsed;
    CHECK(proto::decode_line(proto::encode_request(7, c), parsed) == proto::kOk);
    CHECK(parsed.name == "set_rules_v2" && parsed.set_algos.config_version == 2);
    CHECK(parsed.set_algos.algos[0].rules[1].task_id == 2);
    auto wire = json(proto::encode_request(7, c));
    auto bad = wire;
    bad["cmd"] = "set_algos";
    bad.removeMember("config_version");
    CHECK(proto::decode_line(dump(bad), parsed) == proto::kErrProtocol);
    for (const auto* key : {"task_id", "rule_id", "task_revision", "rule_version"}) {
        bad = wire;
        bad["algos"][0]["rules"][0][key] = 0;
        CHECK(proto::decode_line(dump(bad), parsed) == proto::kErrProtocol);
    }
    bad = wire;
    bad["algos"][0]["rules"][1] = bad["algos"][0]["rules"][0];
    CHECK(proto::decode_line(dump(bad), parsed) == proto::kErrProtocol);
    for (const auto* key : {"type", "thr", "fps", "roi"}) {
        bad = wire;
        if (std::string(key) == "type")
            bad["algos"][0]["rules"][0][key] = "unsupported";
        else if (std::string(key) == "roi")
            bad["algos"][0]["rules"][0][key] = Json::Value(Json::arrayValue);
        else
            bad["algos"][0]["rules"][0][key] = 100;
        CHECK(proto::decode_line(dump(bad), parsed) == proto::kErrProtocol);
    }
    bad = wire;
    bad["unexpected"] = true;
    CHECK(proto::decode_line(dump(bad), parsed) == proto::kErrProtocol);
    auto text = proto::encode_request(7, c);
    CHECK(proto::decode_line(text + " {}", parsed) == proto::kErrProtocol);
    CHECK(proto::decode_line("{\"cmd\":\"get_metrics\",\"cmd\":\"get_capabilities\",\"msg_id\":1}",
                             parsed) == proto::kErrProtocol);
    CHECK(proto::supports_rule_v2(proto::engine_capabilities_json()));
    CHECK(!proto::supports_rule_v2("{}"));
    auto caps = json(proto::engine_capabilities_json());
    caps["independent_sampling"] = false;
    CHECK(!proto::supports_rule_v2(dump(caps)));
    proto::RuleEventEvt event;
    event.id = 1;
    event.algo = "person";
    event.type = "entry";
    event.task_id = 1;
    event.rule_id = 101;
    event.task_revision = 2;
    event.rule_version = 3;
    CHECK(proto::decode_line(proto::encode_event(event), parsed) == proto::kOk);
    CHECK(parsed.event.rule_version == 3);
    bad = json(proto::encode_event(event));
    bad.removeMember("rule_id");
    CHECK(proto::decode_line(dump(bad), parsed) == proto::kErrProtocol);
    event.task_id = event.rule_id = event.task_revision = event.rule_version = 0;
    CHECK(proto::decode_line(proto::encode_event(event), parsed) == proto::kOk &&
          parsed.event.task_id == 0);
}
void isolation() {
    RuleRuntime runtime;
    auto c = config({rule(1), rule(2), rule(3, .95f)});
    auto outside = rule(4);
    outside.roi = {{.7f, .7f}, {1, .7f}, {1, 1}};
    c.algos[0].rules.push_back(outside);
    auto other = c.algos[0];
    other.algo = "vehicle";
    other.rules = {rule(5)};
    c.algos.push_back(other);
    const auto g = runtime.configure(c);
    CHECK(g > 0);
    CHECK(evaluate(runtime, g, 0).events.empty());
    auto fired = evaluate(runtime, g, 100);
    CHECK(fired.accepted && fired.events.size() == 2);
    if (fired.events.size() == 2) {
        CHECK(fired.events[0].task_id == 1 && fired.events[1].task_id == 2);
        CHECK(fired.events[1].rule_version == 3 && fired.events[1].task_revision == 2);
    }
    CHECK(evaluate(runtime, g, 100).accepted == false); // Out-of-order result is rejected.
    CHECK(evaluate(runtime, g, 200).events.empty());    // Independent cooldowns already armed.
    CHECK(evaluate(runtime, g, 0, .8f, "vehicle").events.empty());
    auto vehicle = evaluate(runtime, g, 100, .8f, "vehicle");
    CHECK(vehicle.events.size() == 1 && vehicle.events[0].task_id == 5);
    CHECK(!evaluate(runtime, g, 300, .8f, "unknown").accepted);
    c.algos[0].rules.erase(c.algos[0].rules.begin());
    auto next = runtime.configure(c);
    CHECK(!evaluate(runtime, g, 300).accepted); // Old configuration cannot publish results.
    CHECK(evaluate(runtime, next, 300).events.empty());
    CHECK(evaluate(runtime, next, 400).events.empty()); // Unchanged rule does not fire anew.
    runtime.remove(1);
    CHECK(!evaluate(runtime, next, 500).accepted);
    CHECK(runtime.configure(c) > next); // Camera ID reuse cannot revive old results.
    RuleRuntime low_confidence;
    const auto low_generation = low_confidence.configure(config({rule(8, .2f)}));
    CHECK(evaluate(low_confidence, low_generation, 0, .3f).events.empty());
    CHECK(evaluate(low_confidence, low_generation, 100, .3f).events.size() == 1);
}
void preserve_dwell_and_sampling() {
    RuleRuntime runtime;
    auto a = rule(1, .2f, 10), b = rule(2, .6f, 2);
    b.type = "dwell";
    b.dwell_sec = 2;
    auto c = config({a, b});
    auto g = runtime.configure(c);
    for (int ms = 0; ms <= 1500; ms += 100)
        evaluate(runtime, g, ms);
    // Remove A and lower the shared envelope. B's timer and sampling phase must survive.
    c.algos[0].rules = {b};
    c.algos[0].thr = .6f;
    c.algos[0].fps = 2;
    c.algos[0].rules[0].task_revision = 3;
    g = runtime.configure(c);
    auto fired = evaluate(runtime, g, 2000);
    CHECK(fired.events.empty());
    fired = evaluate(runtime, g, 2500);
    CHECK(fired.events.size() == 1 && fired.events[0].task_id == 2);
    if (!fired.events.empty())
        CHECK(fired.events[0].task_revision == 3);
    CHECK(evaluate(runtime, g, 3000).events.empty());
    RuleRuntime sampling;
    auto slow = rule(10, .5f, 1);
    auto fast = rule(11, .5f, 10);
    auto g2 = sampling.configure(config({slow, fast}));
    CHECK(evaluate(sampling, g2, 0).events.empty());
    CHECK(evaluate(sampling, g2, 100).events.size() == 1);
    auto second = evaluate(sampling, g2, 1000);
    CHECK(second.events.size() == 1 && second.events[0].task_id == 10);
    c = config({rule(1)});
    c.algos[0].fps = 3;
    c.algos[0].rules[0].fps = 3;
    RuleRuntime gate;
    g = gate.configure(c);
    int count = 0;
    for (int ms = 0; ms < 10000; ms += 100)
        if (gate.sample_model(1, "person", g, ms))
            ++count;
    CHECK(count == 30);
    CHECK(gate.begin_frame(1, g, 9000, 1) == g);
    CHECK(gate.begin_frame(1, g, 9000, 1) == 0);
    auto restarted = gate.begin_frame(1, g, 0, 2);
    CHECK(restarted > g);
    CHECK(!gate.sample_model(1, "person", g, 9100));
    CHECK(gate.sample_model(1, "person", restarted, 0));
    CHECK(!evaluate(gate, g, 9100).accepted);
    CHECK(evaluate(gate, restarted, 0).accepted);
}
void persistence_and_compiler() {
    Db db(":memory:");
    CHECK(db.migrate() == Db::kSchemaLatest);
    CameraRow camera;
    camera.name = "provenance";
    camera.rtsp_main = "rtsp://main";
    camera.rtsp_sub = "rtsp://sub";
    auto id = db.insert_camera(camera);
    CHECK(id > 0);
    EventRow e;
    e.camera_id = id;
    e.algo_id = "person";
    e.type = "event";
    e.ts = 1;
    CHECK(db.insert_event(e) > 0);
    e.task_id = 9;
    e.rule_id = 19;
    e.task_revision = 2;
    e.rule_version = 3;
    e.ts = 2;
    auto event_id = db.insert_event(e);
    CHECK(event_id > 0);
    auto rows = db.list_events(id, "person", 0, 0, 10);
    CHECK(rows.size() == 2);
    if (rows.size() == 2) {
        CHECK(rows[0].rule_id == 19 && rows[0].rule_version == 3);
        CHECK(rows[1].task_id == 0);
    }
    CHECK(db.exec("CREATE TRIGGER source_failure BEFORE INSERT ON event_rule_sources BEGIN SELECT "
                  "RAISE(ABORT,'fail'); END;"));
    CHECK(db.insert_event(e) < 0);
    CHECK(db.list_events(0, "", 0, 0, 10).size() == 2); // Atomic rollback.
    CHECK(db.exec("DROP TRIGGER source_failure;"));
    e.rule_id = 0;
    CHECK(db.insert_event(e) < 0); // No partially identified event.
    CHECK(db.exec("DELETE FROM events WHERE id=" + std::to_string(event_id)));
    auto sources = db.prepare("SELECT count(*) FROM event_rule_sources");
    CHECK(sources && sources->step() && sources->i64(0) == 0);
    AlgorithmTask task;
    task.id = 9;
    task.revision = 2;
    task.kind = "v2";
    task.enabled = true;
    task.camera_ids = {id};
    TaskAlgorithm algorithm;
    algorithm.algo_id = "person";
    algorithm.threshold = .4;
    algorithm.fps = 3;
    task.algorithms = {algorithm};
    TaskRule r;
    r.id = 19;
    r.version = 3;
    r.algo_id = "person";
    r.capability_id = "region_in";
    r.roi_json = "[[0,0],[1,0],[1,1]]";
    task.rules = {r};
    auto plan = compile_task_plan({task}, id, 0, false);
    proto::SetAlgos wire;
    std::string error;
    CHECK(compile_task_wire(plan, id, wire, error));
    if (!wire.algos.empty() && !wire.algos[0].rules.empty())
        CHECK(wire.algos[0].rules[0].rule_id == 19);
    else
        CHECK(false);
    plan.errors.push_back("bad configuration");
    CHECK(!compile_task_wire(plan, id, wire, error));
    CHECK(wire.algos.empty());
}
class Infer : public IInferEngine {
    bool load(const std::string&) override {
        return true;
    }
    bool run(const InferTensor&, std::vector<std::vector<float>>& out) override {
        out = {{0, .2f, 1, 1, 2, 2}};
        return true;
    }
};
void dynamic_decode_threshold() {
    auto algo = std::make_shared<AlgoInstance>();
    algo->algo_id = "model";
    algo->infer = std::make_shared<Infer>();
    algo->post = [](const auto&, const auto&) {
        return std::vector<proto::DetTarget>{};
    }; // Manifest .45 would discard .2.
    algo->post_at_threshold = [](const auto& outs, const auto& lb, float threshold) {
        auto targets = AlgoInstance::default_post(outs, lb);
        if (threshold > .2f)
            targets.clear();
        return targets;
    };
    NpuScheduler scheduler;
    scheduler.start();
    for (float threshold : {0.f, .1f, .9f}) {
        std::promise<size_t> completed;
        auto done = completed.get_future();
        Job job;
        job.algo = algo;
        job.explicit_threshold = true;
        job.conf_th = threshold;
        job.cb = [&](auto&& targets) { completed.set_value(targets.size()); };
        scheduler.submit_steady(std::move(job));
        CHECK(done.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        if (done.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            CHECK(done.get() == (threshold > .2f ? 0 : 1));
    }
    scheduler.stop();
}
} // namespace
int main() {
    protocol();
    isolation();
    preserve_dwell_and_sampling();
    persistence_and_compiler();
    dynamic_decode_threshold();
    if (!g_fail)
        printf("rule_runtime PASSED\n");
    return g_fail;
}
