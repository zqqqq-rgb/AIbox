#pragma once
#include "rules/RuleEngine.h"
#include "track/TrackerHub.h"
#include <memory>
#include <mutex>
namespace aibox {
// Thread-safe model/identity isolation over the existing single-rule evaluator.
class RuleRuntime {
public:
    uint64_t configure(const proto::SetAlgos& config);
    uint64_t generation(uint32_t camera) const;
    void remove(uint32_t camera);
    uint64_t begin_frame(uint32_t camera, uint64_t generation, int64_t pts_ms, uint64_t sequence);
    bool sample_model(uint32_t camera, const std::string& model, uint64_t generation,
                      int64_t pts_ms);
    struct Result {
        bool accepted = false;
        std::vector<AlarmEvent> events;
    };
    Result evaluate(uint32_t camera, const std::string& model, uint64_t generation,
                    std::vector<proto::DetTarget>& targets, int64_t pts_ms, uint32_t width,
                    uint32_t height);

private:
    struct Slot {
        std::string fingerprint;
        proto::RuleCfg source;
        bool v2 = false;
        std::string model;
        RuleEngine engine;
        int64_t next_eval_us = -1;
    };
    struct Model {
        float tracker_threshold = .5f;
        uint8_t fps = 1;
        int64_t next_infer_us = -1, last_pts_ms = -1;
        track::ByteTrack tracker;
        std::map<std::pair<uint64_t, uint64_t>, std::shared_ptr<Slot>> rules;
    };
    mutable std::mutex mutex_;
    std::map<uint32_t, uint64_t> generations_;
    std::map<uint32_t, std::map<std::string, std::shared_ptr<Model>>> models_;
    struct FrameClock {
        int64_t pts = -1;
        uint64_t sequence = 0;
    };
    std::map<uint32_t, FrameClock> clocks_;
};
} // namespace aibox
