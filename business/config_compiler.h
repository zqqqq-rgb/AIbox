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

}  // namespace biz
}  // namespace aibox
