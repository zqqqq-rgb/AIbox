#include "rules/RuleRuntime.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace aibox {
namespace {
bool due(int64_t pts_ms, uint8_t fps, int64_t& next_us) {
    if (pts_ms < 0 || pts_ms > (std::numeric_limits<int64_t>::max() - 1000000) / 1000)
        return false;
    const auto now = pts_ms * 1000;
    if (next_us < 0)
        next_us = now;
    if (now < next_us)
        return false;
    const int64_t period = 1000000 / std::max<int>(1, fps);
    next_us += ((now - next_us) / period + 1) * period;
    return true;
}
} // namespace
uint64_t RuleRuntime::configure(const proto::SetAlgos& config) {
    std::string error;
    if (config.config_version == 2 && !proto::validate_v2_config(config, error))
        return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& old = models_[config.id];
    std::map<std::string, std::shared_ptr<Model>> next;
    for (const auto& algorithm : config.algos) {
        auto previous = old.find(algorithm.algo);
        auto model = previous == old.end() ? std::make_shared<Model>() : previous->second;
        if (model->fps != algorithm.fps)
            model->next_infer_us = -1;
        model->fps = algorithm.fps ? algorithm.fps : 1;
        model->tracker_threshold = algorithm.has_rules ? algorithm.thr : .5f;
        std::vector<proto::RuleCfg> rules = algorithm.rules;
        if (!algorithm.has_rules && algorithm.has_roi && algorithm.roi.size() >= 3) {
            proto::RuleCfg r;
            r.type = algorithm.rule_type;
            r.event_type = algorithm.event_type;
            r.roi = algorithm.roi;
            r.threshold = algorithm.thr;
            r.fps = algorithm.fps;
            r.dwell_sec = algorithm.dwell_sec;
            r.debounce_sec = algorithm.debounce_sec;
            rules.push_back(std::move(r));
        }
        std::map<std::pair<uint64_t, uint64_t>, std::shared_ptr<Slot>> retained;
        for (auto source : rules) {
            if (source.event_type.empty())
                source.event_type = algorithm.algo + "_" + source.type;
            auto fingerprint_model = algorithm;
            if (algorithm.has_rules) {
                fingerprint_model.thr = source.threshold;
                fingerprint_model.fps = source.fps;
            }
            fingerprint_model.rules = {source};
            // A sibling rule edit bumps its task revision but must not restart this timer.
            if (algorithm.has_rules)
                fingerprint_model.rules[0].task_revision = 1;
            proto::SetAlgos key;
            key.id = config.id;
            key.config_version = config.config_version;
            key.algos = {fingerprint_model};
            const auto fingerprint = proto::encode_request(1, key);
            const auto identity = std::make_pair(source.task_id, source.rule_id);
            auto existing = model->rules.find(identity);
            std::shared_ptr<Slot> slot;
            if (existing != model->rules.end() && existing->second->fingerprint == fingerprint) {
                slot = existing->second;
                slot->source = source;
            } else {
                slot = std::make_shared<Slot>();
                slot->fingerprint = fingerprint;
                slot->source = source;
                slot->v2 = algorithm.has_rules;
                slot->model = algorithm.algo;
                RuleEngine::Rule rule;
                rule.type =
                    source.type == "dwell" ? RuleEngine::Type::DWELL : RuleEngine::Type::REGION_IN;
                rule.polygon = source.roi;
                rule.event_type = source.event_type;
                rule.dwell_sec = source.dwell_sec;
                rule.debounce_sec = source.debounce_sec;
                rule.algo = algorithm.algo;
                slot->engine.set_rules(config.id, {rule});
            }
            retained[identity] = std::move(slot);
        }
        model->rules = std::move(retained);
        next[algorithm.algo] = std::move(model);
    }
    old = std::move(next);
    return ++generations_[config.id];
}
uint64_t RuleRuntime::generation(uint32_t camera) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = generations_.find(camera);
    return it == generations_.end() ? 0 : it->second;
}
void RuleRuntime::remove(uint32_t camera) {
    std::lock_guard<std::mutex> lock(mutex_);
    models_.erase(camera);
    clocks_.erase(camera);
    ++generations_[camera];
}
uint64_t RuleRuntime::begin_frame(uint32_t camera, uint64_t generation, int64_t pts_ms,
                                  uint64_t sequence) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!generation || generation != generations_[camera] || !models_.count(camera) || pts_ms < 0 ||
        pts_ms > (std::numeric_limits<int64_t>::max() - 1000000) / 1000)
        return 0;
    auto& clock = clocks_[camera];
    if (sequence <= clock.sequence)
        return 0;
    if (clock.pts >= 0 && pts_ms < clock.pts) {
        ++generations_[camera]; // PTS restart invalidates every still-in-flight result.
        for (auto& entry : models_[camera]) {
            auto& model = *entry.second;
            model.next_infer_us = -1;
            model.last_pts_ms = -1;
            model.tracker.reset();
            for (auto& item : model.rules) {
                auto& slot = *item.second;
                slot.next_eval_us = -1;
                slot.engine = RuleEngine{};
                RuleEngine::Rule rule;
                rule.type = slot.source.type == "dwell" ? RuleEngine::Type::DWELL
                                                        : RuleEngine::Type::REGION_IN;
                rule.polygon = slot.source.roi;
                rule.event_type = slot.source.event_type;
                rule.dwell_sec = slot.source.dwell_sec;
                rule.debounce_sec = slot.source.debounce_sec;
                rule.algo = slot.model;
                slot.engine.set_rules(camera, {rule});
            }
        }
    }
    clock.pts = pts_ms;
    clock.sequence = sequence;
    return generations_[camera];
}
bool RuleRuntime::sample_model(uint32_t camera, const std::string& model, uint64_t generation,
                               int64_t pts_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = models_.find(camera);
    if (!generation || generations_[camera] != generation || it == models_.end())
        return false;
    const auto found = it->second.find(model);
    if (found == it->second.end())
        return false;
    return due(pts_ms, found->second->fps, found->second->next_infer_us);
}
RuleRuntime::Result RuleRuntime::evaluate(uint32_t camera, const std::string& name,
                                          uint64_t generation,
                                          std::vector<proto::DetTarget>& targets, int64_t pts_ms,
                                          uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(mutex_);
    Result result;
    const auto cam = models_.find(camera);
    if (!generation || generations_[camera] != generation || cam == models_.end() || !width ||
        !height)
        return result;
    const auto found = cam->second.find(name);
    if (found == cam->second.end())
        return result;
    auto& model = *found->second;
    if (pts_ms < 0 || pts_ms <= model.last_pts_ms ||
        pts_ms > (std::numeric_limits<int64_t>::max() - 1000000) / 1000)
        return result;
    model.last_pts_ms = pts_ms;
    model.tracker.update(targets, pts_ms, model.tracker_threshold);
    result.accepted = true;
    for (auto& entry : model.rules) {
        auto& slot = *entry.second;
        if (slot.v2 && !due(pts_ms, slot.source.fps, slot.next_eval_us))
            continue;
        std::vector<proto::DetTarget> filtered;
        for (const auto& target : targets)
            if (!slot.v2 || (std::isfinite(target.conf) && target.conf >= slot.source.threshold))
                filtered.push_back(target);
        slot.engine.set_frame_size(camera, width, height);
        auto events = slot.engine.eval(camera, filtered, pts_ms);
        for (auto& event : events) {
            event.task_id = slot.source.task_id;
            event.rule_id = slot.source.rule_id;
            event.task_revision = slot.source.task_revision;
            event.rule_version = slot.source.rule_version;
            result.events.push_back(std::move(event));
        }
    }
    return result;
}
} // namespace aibox
