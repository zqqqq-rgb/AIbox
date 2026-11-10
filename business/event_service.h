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
private:
    Db& db_;
};
} }
