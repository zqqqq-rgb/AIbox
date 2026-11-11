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

add_executable(test_rule_runtime test_rule_runtime.cpp)
target_link_libraries(test_rule_runtime PRIVATE aibox_business)
add_test(NAME rule_runtime COMMAND test_rule_runtime)
set_tests_properties(rule_runtime PROPERTIES TIMEOUT 30 LABELS "businessd_phase2")

add_executable(test_engine_v2 test_engine_v2.cpp)
target_link_libraries(test_engine_v2 PRIVATE aibox_business)
add_dependencies(test_engine_v2 core-engine-fake businessd)
add_test(NAME engine_v2 COMMAND test_engine_v2 $<TARGET_FILE:core-engine-fake> $<TARGET_FILE:businessd>)
set_tests_properties(engine_v2 PROPERTIES TIMEOUT 40 LABELS "businessd_phase2")

add_executable(test_task_runtime test_task_runtime.cpp)
target_link_libraries(test_task_runtime PRIVATE aibox_business)
add_test(NAME task_runtime COMMAND test_task_runtime)
set_tests_properties(task_runtime PROPERTIES TIMEOUT 40 LABELS "businessd_phase2")

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

add_executable(test_tasks test_tasks.cpp)
target_link_libraries(test_tasks PRIVATE aibox_business)
add_test(NAME tasks COMMAND test_tasks)
set_tests_properties(tasks PROPERTIES TIMEOUT 30 LABELS "businessd_phase2")

add_executable(test_camera_service test_camera_service.cpp)
target_link_libraries(test_camera_service PRIVATE aibox_business)
add_test(NAME camera_service COMMAND test_camera_service)

add_executable(test_business_startup test_business_startup.cpp)
target_link_libraries(test_business_startup PRIVATE aibox_business)
add_dependencies(test_business_startup businessd)
add_test(NAME business_startup COMMAND test_business_startup $<TARGET_FILE:businessd>)
set_tests_properties(camera_service business_startup PROPERTIES TIMEOUT 30)

add_executable(test_business_recovery test_business_recovery.cpp)
target_link_libraries(test_business_recovery PRIVATE aibox_business)
add_dependencies(test_business_recovery businessd)
add_test(NAME business_recovery COMMAND test_business_recovery $<TARGET_FILE:businessd>)
set_tests_properties(business_recovery PROPERTIES TIMEOUT 90 LABELS "businessd_phase1")
add_test(NAME task_recovery COMMAND test_business_recovery $<TARGET_FILE:businessd> --tasks-only)
set_tests_properties(task_recovery PROPERTIES TIMEOUT 90 LABELS "businessd_phase2")

add_executable(test_uds_client test_uds_client.cpp)
target_link_libraries(test_uds_client PRIVATE aibox_business)
add_test(NAME uds_client COMMAND test_uds_client)

add_executable(test_zlm_client test_zlm_client.cpp)
target_link_libraries(test_zlm_client PRIVATE aibox_business)
add_test(NAME zlm_client COMMAND test_zlm_client)

add_executable(test_http_api test_http_api.cpp)
target_link_libraries(test_http_api PRIVATE aibox_business)
add_test(NAME http_api COMMAND test_http_api)

add_executable(test_event_detail test_event_detail.cpp)
target_link_libraries(test_event_detail PRIVATE aibox_business)
add_test(NAME event_detail COMMAND test_event_detail)

add_executable(test_ws_hub test_ws_hub.cpp)
target_link_libraries(test_ws_hub PRIVATE aibox_business)
add_test(NAME ws_hub COMMAND test_ws_hub)

add_executable(test_ws_limits test_ws_limits.cpp)
target_link_libraries(test_ws_limits PRIVATE aibox_business)
add_test(NAME ws_limits COMMAND test_ws_limits)
set_tests_properties(ws_hub ws_limits PROPERTIES TIMEOUT 30)

add_executable(test_janitor test_janitor.cpp)
target_link_libraries(test_janitor PRIVATE aibox_business)
add_test(NAME janitor COMMAND test_janitor)

add_executable(test_onvif test_onvif.cpp)
target_link_libraries(test_onvif PRIVATE aibox_business)
add_test(NAME onvif COMMAND test_onvif)

add_executable(test_camera_discovery test_camera_discovery.cpp)
target_link_libraries(test_camera_discovery PRIVATE aibox_business)
add_test(NAME camera_discovery COMMAND test_camera_discovery)

add_executable(test_access_session test_access_session.cpp)
target_link_libraries(test_access_session PRIVATE aibox_business)
add_test(NAME access_session COMMAND test_access_session)

add_executable(test_remote_access test_remote_access.cpp)
target_link_libraries(test_remote_access PRIVATE aibox_business)
add_test(NAME remote_access COMMAND test_remote_access)
set_tests_properties(remote_access PROPERTIES TIMEOUT 30)

add_executable(test_event_important test_event_important.cpp)
target_link_libraries(test_event_important PRIVATE aibox_business)
add_test(NAME event_important COMMAND test_event_important)
set_tests_properties(event_important PROPERTIES TIMEOUT 30)

# Phase-one smoke uses Linux businessd and local doubles; no RKNN/board is needed.
set_tests_properties(db config_compiler camera_service business_startup uds_client
    zlm_client http_api ws_hub ws_limits
    PROPERTIES LABELS "businessd_phase1")
set_property(TEST db config_compiler http_api APPEND PROPERTY LABELS "businessd_phase2")
