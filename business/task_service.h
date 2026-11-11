#pragma once
#include "db.h"
#include "task_compiler.h"
#include <json/json.h>
namespace aibox {
namespace biz {
class CameraService;
struct TaskResult {
    int code = 0;
    std::string message = "ok";
    Json::Value data;
};
// Draft editing plus durable transitions through the shared camera configuration executor.
class TaskService {
public:
    explicit TaskService(Db& db, int cores = 2, int64_t memory_kb = 2097152, CameraService* runtime=nullptr)
        : db_(db), cores_(cores), memory_kb_(memory_kb), runtime_(runtime) {}
    Json::Value capabilities();
    Json::Value list();
    TaskResult get(int64_t id);
    TaskResult diagnostics(int64_t id);
    TaskResult save(int64_t id, const Json::Value& body);
    TaskResult remove(int64_t id, const Json::Value& body);
    TaskResult preview(const Json::Value& body);
    TaskResult set_enabled(int64_t id, const Json::Value& body);

private:
    Json::Value describe(const AlgorithmTask& task);
    Db& db_;
    int cores_;
    int64_t memory_kb_;
    CameraService* runtime_;
};
} // namespace biz
} // namespace aibox
