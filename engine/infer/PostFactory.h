// PostFactory —— post_type → 输出解码闭包分发（纯逻辑，vision-only 首版三种）
//
// manifest.post_type 白名单（known_post_types）与分派同步维护；未知类型在
// AlgoPackage 校验期拒绝（disabled），此处兜底返回空闭包。
// 注册表纪律：make_post 返回空闭包 = post_type 与模型输出不匹配 → build 失败
//（宁可 disabled 不可静默用错解码器——坐标错误比无结果更危险）
//
// 首版类型：
//   yolov8_zoo9  rknn zoo 9 输出 YOLOv8/11（DFL decode + NMS + inv_letterbox）
//   det_generic  单输出 [n,6]（cls,conf,x,y,w,h，模型输入系）→ 逆变换 + 裁剪
//                （覆盖自训检测/缺陷检测等非 zoo 布局的 1 输出检测模型）
//   cls_top1     单输出 [1,num_cls] 分类 → argmax（无框：w=h=0，前端按整幅语义消费；
//                覆盖质检 NG 判定/站坐/属性等二次分类）
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "infer/AlgoInstance.h"
#include "model/AlgoPackage.h"

namespace aibox {

// 首版内建类型（AlgoPackage 校验白名单同源）
const std::vector<std::string>& known_post_types();
bool is_known_post_type(const std::string& post_type);

// out_hw：各输出 NCHW [h,w]（RknnEngine::out_hw；仅 yolov8_zoo9 使用，其余类型可传空）
PostFn make_post_by_type(const std::string& post_type, const AlgoManifest& m,
                         const std::vector<std::pair<uint32_t, uint32_t>>& out_hw);

}  // namespace aibox
