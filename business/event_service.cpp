#include "event_service.h"
#include <chrono>
#include <limits>

namespace aibox {
namespace biz {

EventResult EventService::detail(int64_t id) {
    if (id <= 0) return {1001, "expected positive event id", {}};

    // The anchor guarantees one row even for missing events. SQL failures are
    // therefore distinguishable from an absent event without stale error text.
    auto row = db_.prepare(
        "SELECT e.id,e.camera_id,e.algo_id,e.type,e.ts,e.snapshot_path,e.targets_json,e.reported,"
        "COALESCE(s.task_id,0),COALESCE(s.rule_id,0),COALESCE(s.task_revision,0),COALESCE(s.rule_version,0),"
        "e.important,e.important_revision "
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
    data["important"] = row->i64(12) != 0;
    data["important_revision"] = Json::Int64(row->i64(13));
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

EventResult EventService::set_important(int64_t id, const Json::Value& body) {
    if (id <= 0 || !body.isObject() || body.size() != 2 || !body["important"].isBool() ||
        (body["revision"].type() != Json::intValue && body["revision"].type() != Json::uintValue) ||
        !body["revision"].isInt64() || body["revision"].asInt64() < 0)
        return {1001, "expected positive event id, boolean important and nonnegative integer revision", {}};
    const auto revision = body["revision"].asInt64();
    const bool desired = body["important"].asBool();
    // Reserve the writer before reading the revision, including across connections.
    Db::Txn txn(db_, Db::Txn::Mode::Immediate);
    if (!txn.active()) return {5001, "event importance transaction failed", {}};
    auto row = db_.prepare("SELECT e.id,e.important,e.important_revision "
                           "FROM (SELECT 1) anchor LEFT JOIN events e ON e.id=?1;");
    if (!row || !row->bind(1, id) || !row->step())
        return {5001, "event importance query failed", {}};
    if (row->is_null(0)) return {1002, "event not found", {}};
    const bool previous = row->i64(1) != 0;
    const auto current = row->i64(2);
    row.reset();
    Json::Value data(Json::objectValue);
    data["id"] = Json::Int64(id); data["important"] = previous;
    data["important_revision"] = Json::Int64(current); data["changed"] = false;
    if (current != revision) return {1003, "event importance changed; reload detail before saving", data};
    if (previous != desired) {
        if (current == std::numeric_limits<int64_t>::max())
            return {1003, "event importance revision exhausted", data};
        auto update = db_.prepare("UPDATE events SET important=?1,important_revision=?2 WHERE id=?3;");
        if (!update || !update->bind(1, int64_t(desired)) || !update->bind(2, current + 1) ||
            !update->bind(3, id) || !update->run() || db_.changes() != 1)
            return {5001, "event importance update failed", {}};
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto history = db_.prepare("INSERT INTO event_importance_history(event_id,revision,important,changed_at) "
                                   "VALUES(?1,?2,?3,?4);");
        if (!history || !history->bind(1, id) || !history->bind(2, current + 1) ||
            !history->bind(3, int64_t(desired)) || !history->bind(4, now) || !history->run())
            return {5001, "event importance history failed", {}};
        data["important"] = desired; data["important_revision"] = Json::Int64(current + 1);
        data["changed"] = true;
    }
    if (!txn.commit()) return {5001, "event importance commit failed", {}};
    return {0, "ok", data};
}

}  // namespace biz
}  // namespace aibox
