#include "event_service.h"

namespace aibox {
namespace biz {

EventResult EventService::detail(int64_t id) {
    if (id <= 0) return {1001, "expected positive event id", {}};

    // The anchor guarantees one row even for missing events. SQL failures are
    // therefore distinguishable from an absent event without stale error text.
    auto row = db_.prepare(
        "SELECT e.id,e.camera_id,e.algo_id,e.type,e.ts,e.snapshot_path,e.targets_json,e.reported,"
        "COALESCE(s.task_id,0),COALESCE(s.rule_id,0),COALESCE(s.task_revision,0),COALESCE(s.rule_version,0) "
        "FROM (SELECT 1) anchor LEFT JOIN events e ON e.id=?1 "
        "LEFT JOIN event_rule_sources s ON s.event_id=e.id;");
    if (!row || !row->bind(1, id) || !row->step())
        return {5001, "event detail query failed", {}};
    if (row->is_null(0)) return {1002, "event not found", {}};

    Json::Value data(Json::objectValue);
    data["id"] = Json::Int64(row->i64(0));
    data["camera_id"] = Json::Int64(row->i64(1));
    data["algo_id"] = row->text(2);
    data["type"] = row->text(3);
    data["ts"] = Json::Int64(row->i64(4));
    if (!row->text(5).empty()) data["snapshot"] = row->text(5);
    data["reported"] = row->i64(7) != 0;
    const char* sources[] = {"task_id", "rule_id", "task_revision", "rule_version"};
    for (int i = 0; i < 4; ++i) data[sources[i]] = Json::Int64(row->i64(8 + i));

    const auto text = row->text(6);
    Json::CharReaderBuilder builder;
    auto reader = std::unique_ptr<Json::CharReader>(builder.newCharReader());
    Json::Value targets;
    std::string error;
    const bool valid = reader->parse(text.data(), text.data() + text.size(), &targets, &error);
    data["targets"] = valid ? targets : Json::Value();
    data["targets_valid"] = valid;
    return {0, "ok", data};
}

}  // namespace biz
}  // namespace aibox
