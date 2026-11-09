#pragma once
#include "ipc/Protocol.h"
#include "task_types.h"
#include <json/json.h>
#include <map>
namespace aibox {
namespace biz {
struct CompiledTaskPlan {
    std::map<std::string, Json::Value> models;
    std::vector<std::string> errors;
    Json::Value excluded_by_schedule{Json::arrayValue};
};
// Pure control-plane compilation; this result is not a legacy set_algos message.
CompiledTaskPlan compile_task_plan(const std::vector<AlgorithmTask>& tasks, int64_t camera_id,
                                   int64_t at_ms, bool include_drafts);
bool schedule_matches(const TaskSchedule& schedule, int64_t at_ms);
bool valid_task_schedule(const TaskSchedule& schedule);
int64_t task_now_ms();
bool task_window_matches(const AlgorithmTask& task, int64_t at_ms);
// Strict V2 wire conversion; callers must negotiate engine capabilities before applying.
bool compile_task_wire(const CompiledTaskPlan& plan, uint32_t camera, proto::SetAlgos& output,
                       std::string& error);
} // namespace biz
} // namespace aibox
