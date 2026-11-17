#include "event_service.h"
#include <charconv>
#include <chrono>
#include <limits>
#include <set>
#include <string_view>

namespace aibox {
namespace biz {
namespace {

// Status values and transitions are owned by the service. The UI uses the returned options.
constexpr const char* kStatuses[] = {"pending", "acknowledged", "processing", "resolved", "ignored"};
constexpr Json::ArrayIndex kHistoryPageSize = 20;
constexpr Json::ArrayIndex kDispositionBatchLimit = 50;

bool known_status(std::string_view value) {
    for (const auto* status : kStatuses) if (value == status) return true;
    return false;
}

bool parse_cursor(std::string_view value, EventCursor& cursor) {
    if (value.size() > 64 || value.substr(0, 3) != "v1:") return false;
    value.remove_prefix(3);
    auto read_part = [&](int64_t& out, bool last) {
        const auto colon = value.find(':');
        if ((colon == std::string_view::npos) != last) return false;
        const auto part = value.substr(0, colon);
        const auto end = part.data() + part.size();
        const auto parsed = std::from_chars(part.data(), end, out);
        if (parsed.ec != std::errc{} || parsed.ptr != end) return false;
        if (!last) value.remove_prefix(colon + 1);
        return true;
    };
    return read_part(cursor.snapshot_id, false) && read_part(cursor.ts, false) &&
        read_part(cursor.id, true) && cursor.id > 0 && cursor.snapshot_id >= cursor.id;
}

void add_disposition(Json::Value& data, const std::string& status, int64_t revision);

Json::Value event_list_items(const std::vector<EventRow>& events, bool include_disposition_options = false) {
    Json::Value items(Json::arrayValue);
    Json::CharReaderBuilder builder;
    auto reader = std::unique_ptr<Json::CharReader>(builder.newCharReader());
    for (const auto& event : events) {
        Json::Value item;
        item["id"] = Json::Int64(event.id);
        item["camera_id"] = Json::Int64(event.camera_id);
        item["algo_id"] = event.algo_id;
        item["task_id"] = Json::Int64(event.task_id);
        item["rule_id"] = Json::Int64(event.rule_id);
        item["task_revision"] = Json::Int64(event.task_revision);
        item["rule_version"] = Json::Int64(event.rule_version);
        item["important"] = event.important;
        item["important_revision"] = Json::Int64(event.important_revision);
        item["disposition"] = event.disposition;
        if (include_disposition_options) add_disposition(item, event.disposition, event.disposition_revision);
        item["type"] = event.type;
        item["ts"] = Json::Int64(event.ts);
        if (!event.snapshot_path.empty()) item["snapshot"] = event.snapshot_path;
        Json::Value targets;
        std::string errors;
        const auto& text = event.targets_json;
        if (reader->parse(text.data(), text.data() + text.size(), &targets, &errors))
            item["targets"] = std::move(targets);
        items.append(std::move(item));
    }
    return items;
}

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

std::string_view trim_history_text(std::string_view value) {
    // Match browser trim for UTF-8 input, including ideographic space (U+3000).
    constexpr std::string_view spaces[] = {
        " ", "\t", "\r", "\n", u8"\u00a0", u8"\u1680", u8"\u2000", u8"\u2001", u8"\u2002",
        u8"\u2003", u8"\u2004", u8"\u2005", u8"\u2006", u8"\u2007", u8"\u2008", u8"\u2009",
        u8"\u200a", u8"\u2028", u8"\u2029", u8"\u202f", u8"\u205f", u8"\u3000", u8"\ufeff",
    };
    for (bool removed = true; removed;) {
        removed = false;
        for (auto space : spaces) {
            if (value.substr(0, space.size()) == space) {
                value.remove_prefix(space.size());
                removed = true;
            }
            if (value.size() >= space.size() && value.substr(value.size() - space.size()) == space) {
                value.remove_suffix(space.size());
                removed = true;
            }
        }
    }
    return value;
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
    out = trim_history_text(value);
    return true;
}

}  // namespace

Json::Value EventService::list(int64_t camera_id, const std::string& algo_id, int64_t from, int64_t to, int limit) {
    return event_list_items(db_.list_events(camera_id, algo_id, from, to, limit));
}

EventResult EventService::search(EventSearchQuery query, const std::string& cursor) {
    if (query.camera_id < 0 || query.task_id < 0 || query.limit < 1 || query.limit > 100 ||
        query.algo_id.size() > 128 || query.type.size() > 256 ||
        (!query.disposition.empty() && !known_status(query.disposition)) ||
        (query.from_ms && query.to_ms && *query.from_ms > *query.to_ms))
        return {1001, "invalid event search filters", {}};
    if (!cursor.empty()) {
        EventCursor parsed;
        if (!parse_cursor(cursor, parsed)) return {1001, "invalid event cursor", {}};
        query.cursor = parsed;
    }
    EventSearchPage page;
    if (!db_.search_events(query, page)) return {5001, "event search failed", {}};
    Json::Value data;
    data["items"] = event_list_items(page.items, true);
    data["next_cursor"] = Json::Value();
    if (page.next) {
        const auto& next = *page.next;
        data["next_cursor"] = "v1:" + std::to_string(next.snapshot_id) + ":" + std::to_string(next.ts) + ":" + std::to_string(next.id);
    }
    return {0, "ok", data};
}

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
    data["disposition_employee_id_supported"] = true;
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

namespace {

struct DispositionChange {
    std::string desired, operator_name, employee_id, note;
};

EventResult decode_disposition(const Json::Value& body, bool batch, DispositionChange& change) {
    if (!body.isObject() || !body["disposition"].isString())
        return {1001, "expected an event disposition", {}};
    for (const auto& key : body.getMemberNames()) {
        if (key != "disposition" && key != (batch ? "events" : "revision") && key != "operator_name" &&
            key != "operator_employee_id" && key != "note")
            return {1001, "unknown event disposition field", {}};
    }
    if (!read_history_text(body, "operator_name", 120, false, change.operator_name) ||
        !read_history_text(body, "operator_employee_id", 64, false, change.employee_id) ||
        !read_history_text(body, "note", 1000, true, change.note))
        return {1001, "invalid operator name (120 bytes), employee ID (64 bytes), or note (1000 bytes)", {}};
    change.desired = body["disposition"].asString();
    if (!known_status(change.desired) || (batch && change.desired == "pending"))
        return {1001, "unsupported event disposition", {}};
    if (change.desired == "acknowledged" && (change.operator_name.empty() || change.employee_id.empty())) {
        Json::Value data;
        data["reason_code"] = "operator_identity_required";
        return {1001, "operator name and employee ID are required to acknowledge an event", data};
    }
    return {};
}

// Caller owns the write transaction, including the history insert.
EventResult apply_disposition(Db& db, int64_t id, int64_t expected_revision,
                             const DispositionChange& change, int64_t changed_at, bool batch) {
    const auto& desired = change.desired;
    auto row = db.prepare("SELECT e.id,e.disposition,e.disposition_revision "
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
    if (expected_revision != revision) {
        data["reason_code"] = "revision_conflict";
        return {1003, "event disposition changed; reload detail", data};
    }
    if (previous == desired) {
        if (batch || !change.operator_name.empty() || !change.employee_id.empty() || !change.note.empty()) {
            data["reason_code"] = "unchanged_disposition";
            return {1003, "operator and note require a disposition change", data};
        }
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
    auto update = db.prepare("UPDATE events SET disposition=?1,disposition_revision=?2 WHERE id=?3;");
    if (!update || !update->bind(1, desired) || !update->bind(2, revision + 1) ||
        !update->bind(3, id) || !update->run() || db.changes() != 1)
        return {5001, "event disposition update failed", {}};
    auto history = db.prepare("INSERT INTO event_disposition_history"
                               "(event_id,revision,from_status,to_status,changed_at,operator_name,operator_employee_id,note) "
                               "VALUES(?1,?2,?3,?4,?5,NULLIF(?6,''),NULLIF(?7,''),NULLIF(?8,''));");
    if (!history || !history->bind(1, id) || !history->bind(2, revision + 1) ||
        !history->bind(3, previous) || !history->bind(4, desired) || !history->bind(5, changed_at) ||
        !history->bind(6, change.operator_name) || !history->bind(7, change.employee_id) ||
        !history->bind(8, change.note) || !history->run())
        return {5001, "event disposition history failed", {}};
    add_disposition(data, desired, revision + 1);
    data["changed"] = true;
    return {0, "ok", data};
}

}  // namespace

EventResult EventService::set_disposition(int64_t id, const Json::Value& body) {
    if (id <= 0 || !body.isObject() || !is_revision(body["revision"]))
        return {1001, "expected event id, disposition and nonnegative integer revision", {}};
    DispositionChange change;
    const auto decoded = decode_disposition(body, false, change);
    if (decoded.code != 0) return decoded;

    Db::Txn txn(db_, Db::Txn::Mode::Immediate);
    if (!txn.active()) return {5001, "event disposition transaction failed", {}};
    auto result = apply_disposition(db_, id, body["revision"].asInt64(), change, now_ms(), false);
    if (result.code == 0 && !txn.commit()) return {5001, "event disposition commit failed", {}};
    return result;
}

EventResult EventService::set_disposition_batch(const Json::Value& body) {
    DispositionChange change;
    const auto decoded = decode_disposition(body, true, change);
    if (decoded.code != 0) return decoded;
    const auto& events = body["events"];
    if (!events.isArray() || events.empty() || events.size() > kDispositionBatchLimit)
        return {1001, "expected 1 to 50 events", {}};
    std::set<int64_t> ids;
    for (const auto& target : events) {
        if (!target.isObject() || target.size() != 2 || !is_revision(target["id"]) ||
            target["id"].asInt64() == 0 || !is_revision(target["revision"]))
            return {1001, "expected event id and nonnegative integer revision", {}};
        if (!ids.insert(target["id"].asInt64()).second)
            return {1001, "duplicate event id in batch", {}};
    }

    Db::Txn txn(db_, Db::Txn::Mode::Immediate);
    if (!txn.active()) return {5001, "event batch transaction failed", {}};
    Json::Value items(Json::arrayValue);
    const auto changed_at = now_ms();
    for (const auto& target : events) {
        const auto id = target["id"].asInt64();
        auto result = apply_disposition(db_, id, target["revision"].asInt64(), change, changed_at, true);
        if (result.code != 0) {
            result.data["failed_event_id"] = Json::Int64(id);
            return result;
        }
        items.append(std::move(result.data));
    }
    if (!txn.commit()) return {5001, "event batch commit failed", {}};
    Json::Value data;
    data["items"] = std::move(items);
    return {0, "ok", data};
}

EventResult EventService::disposition_history(int64_t id, int64_t before_revision) {
    if (id <= 0 || before_revision < 0) return {1001, "invalid event id or history cursor", {}};
    Db::Txn txn(db_);
    if (!txn.active()) return {5001, "event history transaction failed", {}};
    auto row = db_.prepare("SELECT e.id,h.revision,h.from_status,h.to_status,h.changed_at,h.operator_name,h.note,h.operator_employee_id "
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
        entry["operator_employee_id"] = row->is_null(7) ? Json::Value() : Json::Value(row->text(7));
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
