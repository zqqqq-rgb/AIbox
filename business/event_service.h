#pragma once
#include "db.h"
#include <json/json.h>

namespace aibox { namespace biz {
struct EventResult {
    int code = 0;
    std::string message = "ok";
    Json::Value data;
};
class EventService {
public:
    explicit EventService(Db& db) : db_(db) {}
    EventResult detail(int64_t id);
    EventResult set_important(int64_t id, const Json::Value& body);
    EventResult set_disposition(int64_t id, const Json::Value& body);
    EventResult disposition_history(int64_t id, int64_t before_revision = 0);
private:
    Db& db_;
};
} }
