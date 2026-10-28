# engine/ —— core-engine 三目标（任务分解 §1.2）

# ---- jsoncpp（T1.6 UDS 协议编解码依赖，详设指定）----
find_package(jsoncpp CONFIG QUIET)
if(NOT jsoncpp_FOUND)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(JSONCPP QUIET jsoncpp)
  endif()
endif()
if(NOT jsoncpp_FOUND AND NOT JSONCPP_FOUND)
  message(FATAL_ERROR "未找到 jsoncpp，请安装（Ubuntu: apt install libjsoncpp-dev）")
endif()

# ---- aibox_core：纯逻辑，零硬件头文件（scripts/check-headers.sh 门禁）----
# fakes/ 属纯逻辑测试支撑（SoftLetterbox 兼作 RGA 失败时的 CPU 兜底路径）
# model/ 经 RegistryDeps 工厂注入，零硬件依赖（T3.3 决策：x86 CI 全路径可测）
add_library(aibox_core STATIC
  core/version.cpp
  core/fakes/file_source.cpp
  core/fakes/file_decoder.cpp
  core/fakes/soft_letterbox.cpp
  core/fakes/fake_infer.cpp
  common/EdgeMeter.cpp
  common/FramePool.cpp
  common/LetterboxMath.cpp
  media/Sampler.cpp
  media/CameraChannel.cpp
  infer/NpuCorePlan.cpp
  infer/NpuScheduler.cpp
  infer/Yolov8Post.cpp
  model/AlgoPackage.cpp
  model/ModelRegistry.cpp
  rules/RuleEngine.cpp
  arbiter/DegradeController.cpp
  track/TrackerHub.cpp
  ipc/Protocol.cpp
  ipc/UdsServer.cpp
)
target_include_directories(aibox_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(jsoncpp_FOUND)
  target_link_libraries(aibox_core PUBLIC jsoncpp_lib)
else()
  target_include_directories(aibox_core PUBLIC ${JSONCPP_INCLUDE_DIRS})
  target_link_libraries(aibox_core PUBLIC ${JSONCPP_LIBRARIES})
endif()

# ---- aibox_hw：硬件封装（仅板上编译，默认 OFF）----
# 依赖探测模式复制自 demo（板上已验证）：FFmpeg 走 pkg-config 非 imported，
# MPP/RGA/RKNN 头文件按板上两种常见布局探测 + find_library
if(AIBOX_BUILD_HW)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil)
  message(STATUS "FFMPEG include: ${FFMPEG_INCLUDE_DIRS}")
  message(STATUS "FFMPEG libs: ${FFMPEG_LIBRARIES}")

  find_path(MPP_INCLUDE_DIR rk_mpi.h PATHS /usr/include/rockchip /usr/include)
  find_path(RGA_INCLUDE_DIR im2d.h PATHS /usr/include/rga /usr/include)
  find_path(RKNN_INCLUDE_DIR rknn_api.h PATHS /usr/include /usr/include/rknn)
  if(NOT MPP_INCLUDE_DIR)
    message(FATAL_ERROR "rk_mpi.h not found: 安装 mpp dev (find / -name rk_mpi.h 定位后改此路径)")
  endif()
  if(NOT RGA_INCLUDE_DIR)
    message(FATAL_ERROR "im2d.h not found: 安装 librga-dev 或从 librga 仓库拷头文件")
  endif()
  if(NOT RKNN_INCLUDE_DIR)
    message(FATAL_ERROR "rknn_api.h not found: 从 rknn-toolkit2 仓库 include/ 拷贝")
  endif()

  find_library(MPP_LIB rockchip_mpp REQUIRED)
  find_library(RGA_LIB rga REQUIRED)
  find_library(RKNN_LIB rknnrt REQUIRED)
  message(STATUS "MPP lib: ${MPP_LIB}")
  message(STATUS "RGA lib: ${RGA_LIB}")
  message(STATUS "RKNN lib: ${RKNN_LIB}")

  add_library(aibox_hw STATIC
    hw/RtspPuller.cpp
    hw/MppDecoder.cpp
    hw/RgaPreproc.cpp
    hw/RknnEngine.cpp
    hw/DmaHeapAllocator.cpp
    hw/NpuMetricsSource.cpp
  )
  target_include_directories(aibox_hw PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FFMPEG_INCLUDE_DIRS}
    ${MPP_INCLUDE_DIR}
    ${RGA_INCLUDE_DIR}
    ${RKNN_INCLUDE_DIR}
  )
  target_link_directories(aibox_hw PUBLIC ${FFMPEG_LIBRARY_DIRS} /usr/lib)
  target_link_libraries(aibox_hw PUBLIC
    ${FFMPEG_LIBRARIES}
    ${MPP_LIB} ${RGA_LIB} ${RKNN_LIB}
  )
endif()

# ---- core-engine：core + hw 装配（板端）----
if(AIBOX_BUILD_ENGINE)
  if(NOT AIBOX_BUILD_HW)
    message(FATAL_ERROR "AIBOX_BUILD_ENGINE 需要 AIBOX_BUILD_HW=ON")
  endif()
  add_executable(core-engine main.cpp)
  target_link_libraries(core-engine PRIVATE aibox_core aibox_hw pthread)
endif()

# ---- 板端工具（T3.5 npu_bench 双口径 / T3.2 rga_golden 黄金对照）----
if(AIBOX_BUILD_HW)
  add_executable(npu_bench ../tools/npu_bench.cpp)
  target_link_libraries(npu_bench PRIVATE aibox_core aibox_hw pthread)

  add_executable(rga_golden ../tools/rga_golden.cpp)
  target_link_libraries(rga_golden PRIVATE aibox_core aibox_hw pthread)
endif()
