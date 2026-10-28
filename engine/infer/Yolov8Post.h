// Yolov8Post —— YOLOv8 zoo 9 输出后处理（纯逻辑，T3.3，demo yolov8_post.cpp 移植）
// 来源: rknn_model_zoo examples/yolov8/cpp/postprocess.cc 纯数组版（demo 板上已验证）
// 放入 aibox_core：零硬件依赖，x86 CI 可测（DFL 布局是 demo K7 确定性坑，必须有单测）
//
// 模型输出布局(zoo 版, 9 输出):
//   3 个尺度(80x80 / 40x40 / 20x20), 每尺度 3 个张量:
//     [1,64,h,w] -> bbox xywh 回归(DFL 16bin x 4 边)
//     [1,C,h,w]  -> C 类分数
//     [1,1,h,w]  -> score_sum = Σcls（仅粗筛；最终分数 = 单类最大分，zoo 口径）
// 输出坐标 = 模型输入坐标系；逆变换到原图坐标由 make_yolov8_post 闭包完成(详设 §12.5)
#pragma once

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "infer/AlgoInstance.h"

namespace aibox {

struct YoloBox {
    float x = 0, y = 0, w = 0, h = 0;  // 模型输入坐标系，左上 + 宽高
    float conf = 0;
    int cls = 0;
};

struct YoloPostConfig {
    int num_cls = 80;
    int input_w = 640;
    int input_h = 640;
    float conf_th = 0.45f;   // manifest default_threshold
    float nms_iou = 0.45f;
    int dfl_len = 16;        // DFL: 16 bin x 4 边
    // 3 尺度输出排列: 每尺度 3 张量(顺序 reg64, cls, sum1), 尺度从大到小
    int strides[3] = {8, 16, 32};
    // 类别白名单（manifest.classes）：空 = 全类输出；非空 = 只解码这些类通道
    // （白名单外的类通道直接跳过——省 NMS 且 Tracker/规则/前端只见白名单目标）
    std::vector<int> classes;
};

// inputs: 9 个反量化后 float 数组指针, 顺序
//   [s0.reg, s0.cls, s0.sum, s1.reg, s1.cls, s1.sum, s2.reg, s2.cls, s2.sum]
// dims: 每张量 [h, w]（NCHW dims[2]/dims[3]，RknnEngine::out_hw 提供）
// demo K7 坑内建: reg 是 NCHW [64,h,w]，每个 bin 是独立通道（stride=hw），
// 不得按 stride=1 连续读（会把相邻像素误当 bin → 框坐标全错）
std::vector<YoloBox> yolov8_decode(const float* inputs[9], const int dims[9][2],
                                   const YoloPostConfig& cfg);

// NMS（按类分别做），独立导出便于单测
std::vector<YoloBox> nms_per_class(std::vector<YoloBox>&& cands, float iou_th);

// 组装 AlgoInstance::post 闭包: decode + NMS + inv_letterbox(模型系→原图系) +
// 裁剪到原图边界 + 填充 proto::DetTarget（label=cls，track_id 由 TrackerHub 填）
std::function<std::vector<proto::DetTarget>(const std::vector<std::vector<float>>&,
                                            const LetterBoxParam&)>
make_yolov8_post(YoloPostConfig cfg,
                 const std::vector<std::pair<uint32_t, uint32_t>>& out_hw);

}  // namespace aibox
