#pragma once

#include <string>
#include <vector>

#include "db.h"

namespace aibox {
namespace biz {

// Shared by REST configuration, channel rebuilds and reconnect replay.
// On failure out is empty: invalid ROI must never silently become full frame.
bool compile_algos(uint32_t camera_id, const std::vector<CamAlgoRow>& rows,
                   proto::SetAlgos& out, std::string& error);

struct CameraConfigSnapshot {
    CameraRow camera;
    std::vector<CamAlgoRow> algos;
    bool deleted = false;
    bool has_v2 = false;
    proto::SetAlgos runtime{};
};
struct CompiledCameraConfig {
    proto::AddCam add;
    proto::SetAlgos algos;
};
std::string encode_camera_config(const CameraRow& cam, const std::vector<CamAlgoRow>& algos,
                                  bool deleted = false);
std::string encode_camera_config(const CameraConfigSnapshot& target);
bool build_camera_config(Db& db, const CameraRow& camera, const std::vector<CamAlgoRow>& rows,
                         bool deleted, CameraConfigSnapshot& out, std::string& error,
                         const std::vector<AlgorithmTask>* tasks = nullptr, int64_t at_ms = -1);
bool decode_camera_config(const std::string& text, CameraConfigSnapshot& out);
bool compile_camera_config(const CameraConfigSnapshot& target, const std::string& media_host,
                            int media_port, CompiledCameraConfig& out, std::string& error);

}  // namespace biz
}  // namespace aibox
