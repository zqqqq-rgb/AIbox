#pragma once
#include "db.h"
#include <json/json.h>

namespace aibox { namespace biz {
struct EventResult {
    int code = 0;
    std::string message = "ok";
    Json::Value data;
};
struct EventReceipt {
    int64_t id = -1;
    std::string uid;
    std::string broadcast; // only populated for a newly committed event
};
class EventService {
public:
    explicit EventService(Db& db) : db_(db) {}
    EventReceipt ingest(const proto::RuleEventEvt& event, const std::string& line, bool queue_report);
    Json::Value list(int64_t camera_id, const std::string& algo_id, int64_t from, int64_t to, int limit);
    EventResult search(EventSearchQuery query, const std::string& cursor);
    EventResult stats(const EventFilters& filters);
    EventResult detail(int64_t id);
    EventResult set_important(int64_t id, const Json::Value& body);
    EventResult set_disposition(int64_t id, const Json::Value& body);
    EventResult set_disposition_batch(const Json::Value& body);
    EventResult disposition_history(int64_t id, int64_t before_revision = 0);
private:
    Db& db_;
};
} }
