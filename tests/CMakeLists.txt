# tests/ —— 单元测试（AIBOX_BUILD_TESTS=ON 时由顶层 add_subdirectory）

# T0.3：Fake 四件套最小自测（零外部依赖）
add_executable(test_fakes test_fakes.cpp)
target_link_libraries(test_fakes PRIVATE aibox_core)
add_test(NAME fakes COMMAND test_fakes)

# ---- M1 common 层（§4-T1.x 断言表）----
add_executable(test_edge_meter test_edge_meter.cpp)
target_link_libraries(test_edge_meter PRIVATE aibox_core)
add_test(NAME edge_meter COMMAND test_edge_meter)

add_executable(test_bounded_queue test_bounded_queue.cpp)
target_link_libraries(test_bounded_queue PRIVATE aibox_core)
add_test(NAME bounded_queue COMMAND test_bounded_queue)

add_executable(test_frame_pool test_frame_pool.cpp)
target_link_libraries(test_frame_pool PRIVATE aibox_core)
add_test(NAME frame_pool COMMAND test_frame_pool)

add_executable(test_letterbox_math test_letterbox_math.cpp)
target_link_libraries(test_letterbox_math PRIVATE aibox_core)
add_test(NAME letterbox_math COMMAND test_letterbox_math)

add_executable(test_sampler test_sampler.cpp)
target_link_libraries(test_sampler PRIVATE aibox_core)
add_test(NAME sampler COMMAND test_sampler)

add_executable(test_uds_protocol test_uds_protocol.cpp)
target_link_libraries(test_uds_protocol PRIVATE aibox_core)
add_test(NAME uds_protocol COMMAND test_uds_protocol)

# ---- M2 engine 逻辑层（§4-T2.x 断言表）----
add_executable(test_npu_scheduler test_npu_scheduler.cpp)
target_link_libraries(test_npu_scheduler PRIVATE aibox_core)
add_test(NAME npu_scheduler COMMAND test_npu_scheduler)

# NpuCorePlan 配置面（parse/valid/default/auto 兜底，纯逻辑零硬件）
add_executable(test_npu_core_plan test_npu_core_plan.cpp)
target_link_libraries(test_npu_core_plan PRIVATE aibox_core)
add_test(NAME npu_core_plan COMMAND test_npu_core_plan)

add_executable(test_camera_channel test_camera_channel.cpp)
target_link_libraries(test_camera_channel PRIVATE aibox_core)
add_test(NAME camera_channel COMMAND test_camera_channel)

add_executable(test_rule_engine test_rule_engine.cpp)
target_link_libraries(test_rule_engine PRIVATE aibox_core)
add_test(NAME rule_engine COMMAND test_rule_engine)

add_executable(test_degrade test_degrade.cpp)
target_link_libraries(test_degrade PRIVATE aibox_core)
add_test(NAME degrade COMMAND test_degrade)

add_executable(test_tracker test_tracker.cpp)
target_link_libraries(test_tracker PRIVATE aibox_core)
add_test(NAME tracker COMMAND test_tracker)

add_executable(test_uds_server test_uds_server.cpp)
target_link_libraries(test_uds_server PRIVATE aibox_core)
add_test(NAME uds_server COMMAND test_uds_server)

# ---- M3 core 层（T3.2/T3.3；纯逻辑零硬件依赖）----
add_executable(test_yolov8_post test_yolov8_post.cpp)
target_link_libraries(test_yolov8_post PRIVATE aibox_core)
add_test(NAME yolov8_post COMMAND test_yolov8_post)

add_executable(test_algo_package test_algo_package.cpp)
target_link_libraries(test_algo_package PRIVATE aibox_core)
add_test(NAME algo_package COMMAND test_algo_package)

add_executable(test_model_registry test_model_registry.cpp)
target_link_libraries(test_model_registry PRIVATE aibox_core)
add_test(NAME model_registry COMMAND test_model_registry)

# ---- M4 businessd（§4-T4.x 断言表）----
add_executable(test_db test_db.cpp)
target_link_libraries(test_db PRIVATE aibox_business)
add_test(NAME db COMMAND test_db)

add_executable(test_config_compiler test_config_compiler.cpp)
target_link_libraries(test_config_compiler PRIVATE aibox_business)
add_test(NAME config_compiler COMMAND test_config_compiler)

add_executable(test_uds_client test_uds_client.cpp)
target_link_libraries(test_uds_client PRIVATE aibox_business)
add_test(NAME uds_client COMMAND test_uds_client)

add_executable(test_zlm_client test_zlm_client.cpp)
target_link_libraries(test_zlm_client PRIVATE aibox_business)
add_test(NAME zlm_client COMMAND test_zlm_client)

add_executable(test_http_api test_http_api.cpp)
target_link_libraries(test_http_api PRIVATE aibox_business)
add_test(NAME http_api COMMAND test_http_api)

add_executable(test_ws_hub test_ws_hub.cpp)
target_link_libraries(test_ws_hub PRIVATE aibox_business)
add_test(NAME ws_hub COMMAND test_ws_hub)

add_executable(test_janitor test_janitor.cpp)
target_link_libraries(test_janitor PRIVATE aibox_business)
add_test(NAME janitor COMMAND test_janitor)

add_executable(test_onvif test_onvif.cpp)
target_link_libraries(test_onvif PRIVATE aibox_business)
add_test(NAME onvif COMMAND test_onvif)
