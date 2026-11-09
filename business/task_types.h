#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace aibox {
namespace biz {
struct TaskAlgorithm {
    std::string algo_id;
    double threshold = 0.45;
    int fps = 3;
    bool enabled = true;
    int roi_display = 1;
    std::string roi_color;
};
struct TaskRule {
    int64_t id = 0, version = 1;
    std::string key, algo_id, capability_id, roi_json, params_json;
    bool enabled = true;
};
struct TaskSchedule {
    std::string timezone = "UTC";
    int weekdays = 127; // bit 0 = Monday; overnight windows belong to their start day.
    int start_minute = 0, end_minute = 1440;
};
struct AlgorithmTask {
    int64_t id = 0, revision = 0, legacy_camera_id = 0, deleted_at = 0;
    std::string name, kind = "v2";
    bool enabled = false;
    std::vector<int64_t> camera_ids;
    std::vector<TaskAlgorithm> algorithms;
    std::vector<TaskRule> rules;
    std::vector<TaskSchedule> schedules;
};
struct Capability {
    std::string id, name, rule_type;
    int version = 1;
};
} // namespace biz
} // namespace aibox
