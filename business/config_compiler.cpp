#include "config_compiler.h"

#include <cmath>
#include <memory>
#include <set>

#include <json/json.h>

namespace aibox {
namespace biz {
namespace {
bool parse(const std::string& text, Json::Value& value) {
    Json::CharReaderBuilder builder;
    builder["failIfExtra"] = true;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string error;
    return reader->parse(text.data(), text.data() + text.size(), &value, &error);
}
}  // namespace

bool compile_algos(uint32_t camera_id, const std::vector<CamAlgoRow>& rows,
                   proto::SetAlgos& out, std::string& error) {
    out = {};
    out.id = camera_id;
    error.clear();
    proto::SetAlgos compiled;
    compiled.id = camera_id;
    std::set<std::string> ids;
    for (const auto& row : rows) {
        auto fail = [&](const char* reason) {
            error = row.algo_id + ": " + reason;
            return false;
        };
        if (row.algo_id.empty() || !ids.insert(row.algo_id).second)
            return fail("empty or duplicate algorithm id");
        if (!row.enabled) continue;
        if (!std::isfinite(row.threshold) || row.threshold < 0 || row.threshold > 1)
            return fail("threshold must be finite and in [0,1]");
        if (row.detect_fps < 1 || row.detect_fps > 25)
            return fail("fps must be in [1,25]");
        proto::AlgoCfg cfg;
        cfg.algo = row.algo_id;
        cfg.thr = static_cast<float>(row.threshold);
        cfg.fps = static_cast<uint8_t>(row.detect_fps);
        if (!row.roi_json.empty()) {
            Json::Value roi;
            if (!parse(row.roi_json, roi) || !roi.isArray() || roi.size() < 3)
                return fail("ROI must contain at least three normalized points");
            for (const auto& point : roi) {
                if (!point.isArray() || point.size() != 2 ||
                    !point[0].isNumeric() || !point[1].isNumeric())
                    return fail("invalid ROI point");
                const double x = point[0].asDouble(), y = point[1].asDouble();
                if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 ||
                    y < 0 || y > 1) return fail("ROI point outside [0,1]");
                cfg.roi.emplace_back(static_cast<float>(x), static_cast<float>(y));
            }
            double area = 0;
            for (size_t i = 0; i < cfg.roi.size(); ++i) {
                const auto& a = cfg.roi[i];
                const auto& b = cfg.roi[(i + 1) % cfg.roi.size()];
                area += a.first * b.second - b.first * a.second;
            }
            if (std::abs(area) < 1e-9) return fail("ROI has zero area");
            cfg.has_roi = true;
        }
        if (!row.rule_json.empty()) {
            Json::Value rule;
            if (!parse(row.rule_json, rule) || !rule.isObject())
                return fail("rule must be a JSON object");
            if (rule.isMember("rule_type")) {
                if (!rule["rule_type"].isString()) return fail("invalid rule_type");
                cfg.rule_type = rule["rule_type"].asString();
            }
            if (cfg.rule_type != "region_in" && cfg.rule_type != "dwell")
                return fail("unsupported rule_type");
            if (rule.isMember("event_type")) {
                if (!rule["event_type"].isString()) return fail("invalid event_type");
                cfg.event_type = rule["event_type"].asString();
            }
            if (rule.isMember("dwell_sec")) {
                if (!rule["dwell_sec"].isUInt() || rule["dwell_sec"].asUInt() == 0)
                    return fail("dwell_sec must be positive");
                cfg.dwell_sec = rule["dwell_sec"].asUInt();
            }
            if (rule.isMember("debounce_sec")) {
                if (!rule["debounce_sec"].isUInt()) return fail("invalid debounce_sec");
                cfg.debounce_sec = rule["debounce_sec"].asUInt();
            }
        }
        compiled.algos.push_back(std::move(cfg));
    }
    out = std::move(compiled);
    return true;
}
}  // namespace biz
}  // namespace aibox
