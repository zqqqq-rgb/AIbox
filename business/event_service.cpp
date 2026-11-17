#include "event_service.h"
#include <chrono>
#include <limits>

namespace aibox {
namespace biz {
namespace {

// Status values and transitions are owned by the service. The UI uses the returned options.
constexpr const char* kStatuses[] = {"pending", "acknowledged", "processing", "resolved", "ignored"};
constexpr Json::ArrayIndex kHistoryPageSize = 20;

Json::Value next_dispositions(const std::string& status) {
    Json::Value allowed(Json::arrayValue);
    if (status == "pending") allowed.append("acknowledged");
    if (status == "acknowledged") allowed.append("processing");
    if (status == "processing") allowed.append("resolved");
    if (status == "pending" || status == "acknowledged" || status == "processing")
        allowed.append("ignored");
    if (status == "resolved" || status == "ignored") allowed.append("pending");
    return allowed;
}

void add_disposition(Json::Value& data, const std::string& status, int64_t revision) {
    data["disposition"] = status;
    data["disposition_revision"] = Json::Int64(revision);
    data["allowed_dispositions"] = next_dispositions(status);
}

bool is_revision(const Json::Value& value) {
    return (value.type() == Json::intValue || value.type() == Json::uintValue) &&
        value.isInt64() && value.asInt64() >= 0;
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool read_history_text(const Json::Value& body, const char* key, size_t max_bytes,
                       bool multiline, std::string& out) {
    if (!body.isMember(key)) return true;
    if (!body[key].isString()) return false;
    auto value = body[key].asString();
    if (value.size() > max_bytes) return false;
    for (unsigned char c : value) {
        if ((c < 32 || c == 127) && !(multiline && (c == '\n' || c == '\r' || c == '\t')))
            return false;
    }
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start != std::string::npos)
        out = value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1);
    return true;
}

}  // namespace

EventResult EventService::detail(int64_t id) {
    if (id <= 0) return {1001, "expected positive event id", {}};

    // The anchor guarantees one row even for missing events. SQL failures are
    // therefore distinguishable from an absent event without stale error text.
    auto row = db_.prepare(
        "SELECT e.id,e.camera_id,e.algo_id,e.type,e.ts,e.snapshot_path,e.targets_json,e.reported,"
        "COALESCE(s.task_id,0),COALESCE(s.rule_id,0),COALESCE(s.task_revision,0),COALESCE(s.rule_version,0),"
        "e.important,e.important_revision,e.disposition,e.disposition_revision "
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
    add_disposition(data, row->text(14), row->i64(15));
    data["disposition_history_supported"] = true;
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
        !is_revision(body["revision"]))
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
        auto history = db_.prepare("INSERT INTO event_importance_history(event_id,revision,important,changed_at) "
                                   "VALUES(?1,?2,?3,?4);");
        if (!history || !history->bind(1, id) || !history->bind(2, current + 1) ||
            !history->bind(3, int64_t(desired)) || !history->bind(4, now_ms()) || !history->run())
            return {5001, "event importance history failed", {}};
        data["important"] = desired; data["important_revision"] = Json::Int64(current + 1);
        data["changed"] = true;
    }
    if (!txn.commit()) return {5001, "event importance commit failed", {}};
    return {0, "ok", data};
}

EventResult EventService::set_disposition(int64_t id, const Json::Value& body) {
    if (id <= 0 || !body.isObject() || !body["disposition"].isString() ||
        !is_revision(body["revision"]))
        return {1001, "expected event id, disposition and nonnegative integer revision", {}};
    for (const auto& key : body.getMemberNames()) {
        if (key != "disposition" && key != "revision" && key != "operator_name" && key != "note")
            return {1001, "unknown event disposition field", {}};
    }
    std::string operator_name, note;
    if (!read_history_text(body, "operator_name", 120, false, operator_name) ||
        !read_history_text(body, "note", 1000, true, note))
        return {1001, "invalid operator name (120 bytes) or note (1000 bytes)", {}};
    const auto desired = body["disposition"].asString();
    bool valid_status = false;
    for (const auto* status : kStatuses) if (desired == status) valid_status = true;
    if (!valid_status) return {1001, "unknown event disposition", {}};

    Db::Txn txn(db_, Db::Txn::Mode::Immediate);
    if (!txn.active()) return {5001, "event disposition transaction failed", {}};
    auto row = db_.prepare("SELECT e.id,e.disposition,e.disposition_revision "
                           "FROM (SELECT 1) anchor LEFT JOIN events e ON e.id=?1;");
    if (!row || !row->bind(1, id) || !row->step())
        return {5001, "event disposition query failed", {}};
    if (row->is_null(0)) return {1002, "event not found", {}};
    const auto previous = row->text(1);
    const auto revision = row->i64(2);
    row.reset();

    Json::Value data;
    data["id"] = Json::Int64(id);
    data["changed"] = false;
    add_disposition(data, previous, revision);
    if (body["revision"].asInt64() != revision) {
        data["reason_code"] = "revision_conflict";
        return {1003, "event disposition changed; reload detail", data};
    }
    if (previous == desired) {
        if (!operator_name.empty() || !note.empty()) {
            data["reason_code"] = "unchanged_disposition";
            return {1003, "operator and note require a disposition change", data};
        }
        if (!txn.commit()) return {5001, "event disposition commit failed", {}};
        return {0, "ok", data};
    }
    bool allowed = false;
    for (const auto& status : data["allowed_dispositions"]) if (status.asString() == desired) allowed = true;
    if (!allowed) {
        data["reason_code"] = "transition_not_allowed";
        return {1003, "event disposition transition not allowed", data};
    }
    if (revision == std::numeric_limits<int64_t>::max()) {
        data["reason_code"] = "revision_exhausted";
        return {1003, "event disposition revision exhausted", data};
    }
    auto update = db_.prepare("UPDATE events SET disposition=?1,disposition_revision=?2 WHERE id=?3;");
    if (!update || !update->bind(1, desired) || !update->bind(2, revision + 1) ||
        !update->bind(3, id) || !update->run() || db_.changes() != 1)
        return {5001, "event disposition update failed", {}};
    auto history = db_.prepare("INSERT INTO event_disposition_history"
                               "(event_id,revision,from_status,to_status,changed_at,operator_name,note) "
                               "VALUES(?1,?2,?3,?4,?5,NULLIF(?6,''),NULLIF(?7,''));");
    if (!history || !history->bind(1, id) || !history->bind(2, revision + 1) ||
        !history->bind(3, previous) || !history->bind(4, desired) || !history->bind(5, now_ms()) ||
        !history->bind(6, operator_name) || !history->bind(7, note) || !history->run())
        return {5001, "event disposition history failed", {}};
    if (!txn.commit()) return {5001, "event disposition commit failed", {}};
    add_disposition(data, desired, revision + 1);
    data["changed"] = true;
    return {0, "ok", data};
}

EventResult EventService::disposition_history(int64_t id, int64_t before_revision) {
    if (id <= 0 || before_revision < 0) return {1001, "invalid event id or history cursor", {}};
    Db::Txn txn(db_);
    if (!txn.active()) return {5001, "event history transaction failed", {}};
    auto row = db_.prepare("SELECT e.id,h.revision,h.from_status,h.to_status,h.changed_at,h.operator_name,h.note "
                           "FROM (SELECT 1) anchor LEFT JOIN events e ON e.id=?1 "
                           "LEFT JOIN event_disposition_history h ON h.event_id=e.id AND h.revision<=?2 "
                           "ORDER BY h.revision DESC LIMIT ?3;");
    const auto max_revision = before_revision ? before_revision - 1 : std::numeric_limits<int64_t>::max();
    if (!row || !row->bind(1, id) || !row->bind(2, max_revision) ||
        !row->bind(3, int64_t(kHistoryPageSize + 1)) || !row->step())
        return {5001, "event history query failed", {}};
    if (row->is_null(0)) return {1002, "event not found", {}};

    Json::Value entries(Json::arrayValue);
    Json::Value next;
    do {
        if (row->is_null(1)) break;
        if (entries.size() == kHistoryPageSize) {
            next = entries[entries.size() - 1]["revision"];
            break;
        }
        Json::Value entry;
        entry["revision"] = Json::Int64(row->i64(1));
        entry["from_status"] = row->text(2);
        entry["to_status"] = row->text(3);
        entry["changed_at"] = Json::Int64(row->i64(4));
        entry["operator_name"] = row->is_null(5) ? Json::Value() : Json::Value(row->text(5));
        entry["note"] = row->is_null(6) ? Json::Value() : Json::Value(row->text(6));
        entries.append(std::move(entry));
    } while (row->step());
    if (!db_.last_error().empty()) return {5001, "event history query failed", {}};
    row.reset();
    if (!txn.commit()) return {5001, "event history commit failed", {}};
    Json::Value data;
    data["entries"] = std::move(entries);
    data["next_before_revision"] = std::move(next);
    return {0, "ok", data};
}

}  // namespace biz
}  // namespace aibox
