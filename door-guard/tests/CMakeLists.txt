# tests/ — ctest 用例(宿主 gcc,dg-test 驱动)
#
# 每个测试一个可执行文件、一个 main:失败定位直接到 文件:行号;
# TIMEOUT 防死锁挂死闲时任务。demo_* 是模块使用示例,作为冒烟用例一并跑。
set(DG_TEST_TIMEOUT 120)

macro(dg_add_test name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT ${DG_TEST_TIMEOUT})
endmacro()

dg_add_test(test_event_bus dg_event_bus)
dg_add_test(test_event_bus_stress dg_event_bus)
dg_add_test(test_tasker dg_tasker)
dg_add_test(test_holder dg_holder)
# include 被测 .c 直驱 static 状态机:相机事实用桩,依赖挂真库
dg_add_test(test_capture_health dg_event_bus dg_tasker dg_holder dg_timeutil dg_proto)
dg_add_test(test_registry dg_registry)
dg_add_test(test_proto dg_proto)
dg_add_test(test_valid dg_proto)
dg_add_test(test_storage dg_storage)
set_tests_properties(test_storage PROPERTIES TIMEOUT 300)
dg_add_test(test_feat_cache dg_storage)
set_tests_properties(test_feat_cache PROPERTIES TIMEOUT 60)
dg_add_test(test_cfg dg_config)
set_tests_properties(test_cfg PROPERTIES TIMEOUT 60)
dg_add_test(test_auth_fsm dg_access)
set_tests_properties(test_auth_fsm PROPERTIES TIMEOUT 60)
dg_add_test(test_liveness dg_liveness dg_log)
set_tests_properties(test_liveness PROPERTIES TIMEOUT 60)
dg_add_test(test_uart_mock dg_uart)

dg_add_test(test_fp_proto dg_fp_as608)
set_tests_properties(test_fp_proto PROPERTIES TIMEOUT 60)

# 指纹/IC 应用层(2026-10-01):协议纯函数 / 防重窗 / FSM 分支 / 两条录入链
dg_add_test(test_iccard_proto dg_iccard)
dg_add_test(test_card_dedup dg_card)
dg_add_test(test_fsm_ic dg_access)
set_tests_properties(test_fsm_ic PROPERTIES TIMEOUT 60)
dg_add_test(test_fsm_finger dg_access)
set_tests_properties(test_fsm_finger PROPERTIES TIMEOUT 60)

# IC 绑卡端到端(sim 后端注入帧;覆盖绑定/查重/取消/FLUSH/解绑)
dg_add_test(test_enroll_ic dg_enroll dg_card dg_iccard dg_storage dg_config
                             dg_tasker dg_event_bus dg_proto cjson)
set_tests_properties(test_enroll_ic PROPERTIES TIMEOUT 120)

# 指纹录入端到端(假模组注入应答帧;覆盖 happy/查重/LIMIT/FULL/重采/取消/删除)
dg_add_test(test_fp_enroll dg_fp dg_enroll dg_storage dg_config dg_tasker
                            dg_event_bus dg_proto cjson)
set_tests_properties(test_fp_enroll PROPERTIES TIMEOUT 120)
# 按压确认窗(2026-10-13 口径:短于 finger.press_hold_ms 的保持=假按压,
# 不出 PRESSED/不进采集;覆盖默认 500/短保持丢弃/长保持放行/置 0 钳下限)
dg_add_test(test_fp_press_gate dg_fp dg_storage dg_config dg_tasker
                               dg_event_bus dg_proto cjson)
set_tests_properties(test_fp_press_gate PROPERTIES TIMEOUT 60)
# 指纹两侧数量一致性策略(纯判定边界 0/0、0/N、N/0、N/M + 告知优先级)
dg_add_test(test_fp_sync dg_fp_sync)
dg_add_test(test_rknn_face dg_rknn_face)
dg_add_test(test_face_quality dg_rknn_face)
# 反欺骗纯算法件(crop 几何/通道/中位数平滑,宿主可测)
dg_add_test(test_antispoof dg_rknn_face)
# 头像 JPEG 编解码(往返保真/缩放解码/损坏输入不崩)
dg_add_test(test_jpeg dg_jpeg)
# letterbox 是头内联的纯数学,测试只需那个头目录(不需要 dg_npu:它含 RGA,宿主编不了)
dg_add_test(test_npu_pre dg_rknn_face)

# 录入链路端到端(sim mock 后端应答抓取):请求→回执→DB 状态一致
dg_add_test(test_enroll_flow dg_enroll dg_vision dg_storage dg_config
                             dg_tasker dg_event_bus dg_proto cjson)
set_tests_properties(test_enroll_flow PROPERTIES TIMEOUT 120)

# 系统控制(重启请求链路;宿主 DG_SYSCTL_FAKE 模拟,不真重启)
dg_add_test(test_sysctl dg_sysctl dg_event_bus dg_proto)
target_include_directories(test_npu_pre PRIVATE ${CMAKE_SOURCE_DIR}/drv/npu)
add_executable(test_e2e test_e2e.c)
target_include_directories(test_e2e PRIVATE ${CMAKE_SOURCE_DIR}/services/access ${CMAKE_SOURCE_DIR}/services/vision)
target_link_libraries(test_e2e PRIVATE dg_access dg_enroll dg_vision dg_capture dg_storage dg_config dg_tasker dg_event_bus dg_proto cjson)
add_test(NAME test_e2e COMMAND test_e2e)
set_tests_properties(test_e2e PROPERTIES TIMEOUT 120)

# 视觉工作模式联动(FSM 状态 → mode:误触发开门的第二道门禁)
add_executable(test_vision_mode test_vision_mode.c)
target_include_directories(test_vision_mode PRIVATE ${CMAKE_SOURCE_DIR}/services/access ${CMAKE_SOURCE_DIR}/services/vision)
target_link_libraries(test_vision_mode PRIVATE dg_access dg_vision dg_storage dg_config dg_tasker dg_event_bus dg_proto cjson)
add_test(NAME test_vision_mode COMMAND test_vision_mode)
set_tests_properties(test_vision_mode PROPERTIES TIMEOUT 60)

# 视觉后端契约(可插拔:换模型/换框架不改服务层;假后端驱动)
add_executable(test_vision_backend test_vision_backend.c)
target_include_directories(test_vision_backend PRIVATE ${CMAKE_SOURCE_DIR}/services/vision)
target_link_libraries(test_vision_backend PRIVATE dg_vision dg_storage dg_config dg_tasker dg_event_bus dg_proto cjson)
add_test(NAME test_vision_backend COMMAND test_vision_backend)
set_tests_properties(test_vision_backend PROPERTIES TIMEOUT 60)

# 验证按钮全流程(UI 可见事件序列:ID 框→方式选择→密码框→结果弹窗;含菜单入口)
add_executable(test_verify_flow test_verify_flow.c)
target_include_directories(test_verify_flow PRIVATE ${CMAKE_SOURCE_DIR}/services/access ${CMAKE_SOURCE_DIR}/services/vision)
target_link_libraries(test_verify_flow PRIVATE dg_access dg_vision dg_storage dg_config dg_tasker dg_event_bus dg_proto cjson)
add_test(NAME test_verify_flow COMMAND test_verify_flow)
set_tests_properties(test_verify_flow PROPERTIES TIMEOUT 120)

# mDNS 报文编解码(纯函数:名字/压缩指针/记录写入,不对 socket)
add_executable(test_mdns_wire test_mdns_wire.c
    ${CMAKE_SOURCE_DIR}/services/mdns/mdns_wire.c)
target_include_directories(test_mdns_wire PRIVATE
    ${CMAKE_SOURCE_DIR}/services/mdns ${CMAKE_SOURCE_DIR}/proto)
add_test(NAME test_mdns_wire COMMAND test_mdns_wire)
set_tests_properties(test_mdns_wire PROPERTIES TIMEOUT 30)

# web 凭据与会话(直接编入被测源文件,纯逻辑单测)
add_executable(test_web_auth test_web_auth.c
    ${CMAKE_SOURCE_DIR}/services/web/web_auth.c
    ${CMAKE_SOURCE_DIR}/services/web/web_session.c)
target_include_directories(test_web_auth PRIVATE ${CMAKE_SOURCE_DIR}/services/web)
target_link_libraries(test_web_auth PRIVATE dg_storage dg_proto dg_timeutil crypto pthread)
add_test(NAME test_web_auth COMMAND test_web_auth)
set_tests_properties(test_web_auth PROPERTIES TIMEOUT 60)

# netcore:统一网络事件循环(启停/心跳/post 闭包投递;不建真实连接)
# netcore 与 mongoose 直连被测源文件(不整链 dg_net,保持单测最小依赖)
add_executable(test_netcore test_netcore.c
    ${CMAKE_SOURCE_DIR}/modules/net/netcore.c)
target_include_directories(test_netcore PRIVATE
    ${CMAKE_SOURCE_DIR}/modules/net ${CMAKE_SOURCE_DIR}/third_party/mongoose
    ${CMAKE_SOURCE_DIR}/proto ${CMAKE_SOURCE_DIR}/components/logger)
target_link_libraries(test_netcore PRIVATE mongoose dg_log dg_timeutil pthread)
add_test(NAME test_netcore COMMAND test_netcore)
set_tests_properties(test_netcore PROPERTIES TIMEOUT 30)

# net_info/net_cfg:地址快照兜底(0.0.0.0)与配置校验纯逻辑(宿主);
# apply() 会真改宿主网络,不在单测执行——推板后的板上验收项
add_executable(test_net_info test_net_info.c
    ${CMAKE_SOURCE_DIR}/modules/net/net_info.c
    ${CMAKE_SOURCE_DIR}/modules/net/net_cfg.c)
target_include_directories(test_net_info PRIVATE
    ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/proto ${CMAKE_SOURCE_DIR}/services/config
    ${CMAKE_SOURCE_DIR}/modules/net)
target_link_libraries(test_net_info PRIVATE dg_log dg_config dg_timeutil)
add_test(NAME test_net_info COMMAND test_net_info)
set_tests_properties(test_net_info PROPERTIES TIMEOUT 30)

# OTA 按需写线程流水线(环形缓冲/校验闭环/断点续传/进度事件;DG_OTA_DIR=临时目录)
add_executable(test_ota test_ota.c)
target_include_directories(test_ota PRIVATE
    ${CMAKE_SOURCE_DIR}/services/ota ${CMAKE_SOURCE_DIR}/proto
    ${CMAKE_SOURCE_DIR}/components/event_bus)
target_link_libraries(test_ota PRIVATE dg_net crypto pthread)
add_test(NAME test_ota COMMAND test_ota)
set_tests_properties(test_ota PROPERTIES TIMEOUT 120)

# OTA HTTP 客户端(手写协议层):回环假源(2026-10-25 B8-4)
add_executable(test_ota_http test_ota_http.c)
target_include_directories(test_ota_http PRIVATE
    ${CMAKE_SOURCE_DIR}/services/ota ${CMAKE_SOURCE_DIR}/proto)
target_link_libraries(test_ota_http PRIVATE dg_net crypto pthread)  # OTA 源在 dg_net(无独立 dg_ota)
add_test(NAME test_ota_http COMMAND test_ota_http)
set_tests_properties(test_ota_http PROPERTIES TIMEOUT 30)

# NTP 服务 socket 层:回环 SNTP 假源(2026-10-25 B8-5;离线环境 SKIP)
add_executable(test_ntp test_ntp.c)
target_include_directories(test_ntp PRIVATE
    ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/proto
    ${CMAKE_SOURCE_DIR}/services/ntp ${CMAKE_SOURCE_DIR}/services/config)
target_link_libraries(test_ntp PRIVATE dg_net dg_config dg_timeutil dg_event_bus dg_log)
add_test(NAME test_ntp COMMAND test_ntp)
set_tests_properties(test_ntp PROPERTIES TIMEOUT 60)

# mDNS 应答器 socket 层:回环单播查询(2026-10-25 B8-5;非 root/无组播 SKIP)
add_executable(test_mdns_resp test_mdns_resp.c)
target_include_directories(test_mdns_resp PRIVATE
    ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/proto
    ${CMAKE_SOURCE_DIR}/services/mdns ${CMAKE_SOURCE_DIR}/services/config)
target_link_libraries(test_mdns_resp PRIVATE dg_net dg_storage dg_config dg_log)
add_test(NAME test_mdns_resp COMMAND test_mdns_resp)
set_tests_properties(test_mdns_resp PROPERTIES TIMEOUT 30)

# i18n 一致性:扫描源码 _() 键 + 双 json 覆盖 + 裸中文检查 + 字体字形覆盖
# (C3:随 LVGL9 编译,lv_font_get_glyph_dsc 两代同签名;头路径按代际切换)
add_executable(test_i18n test_i18n.c)
target_compile_definitions(test_i18n PRIVATE DG_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(test_i18n PRIVATE ${CMAKE_SOURCE_DIR}/third_party/lvgl9)
target_link_libraries(test_i18n PRIVATE lvgl dg_ui dg_net cjson)
add_test(NAME test_i18n COMMAND test_i18n)
set_tests_properties(test_i18n PROPERTIES TIMEOUT 60)

# widget 冒烟:无头 LVGL 下真实 widget 创建/交互/弹窗生命周期
add_executable(test_widgets test_widgets.c)
target_compile_definitions(test_widgets PRIVATE DG_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_widgets PRIVATE lvgl dg_ui dg_net)
add_test(NAME test_widgets COMMAND test_widgets)
set_tests_properties(test_widgets PROPERTIES TIMEOUT 60)

# 模块 demo 冒烟(使用示例须始终可运行)
macro(dg_add_demo name src)
    add_executable(${name} ${src})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/proto)
    target_link_libraries(${name} PRIVATE ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
endmacro()

dg_add_demo(demo_event_bus ${CMAKE_SOURCE_DIR}/components/event_bus/demo_event_bus.c dg_event_bus)
dg_add_demo(demo_tasker ${CMAKE_SOURCE_DIR}/components/tasker/demo_tasker.c dg_tasker)
dg_add_demo(demo_holder ${CMAKE_SOURCE_DIR}/components/holder/demo_holder.c dg_holder)

# 触摸 evdev 解析纯逻辑(板上 fts_ts/goodix 事件协议;宿主无设备直测解析)
add_executable(test_touch_evdev test_touch_evdev.c
               ${CMAKE_SOURCE_DIR}/modules/display/touch_evdev.c
               ${CMAKE_SOURCE_DIR}/modules/display/touch_activity.c)
target_include_directories(test_touch_evdev PRIVATE
    ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/proto
    ${CMAKE_SOURCE_DIR}/third_party/lvgl9)
# dg_ui:lv_conf9 的 LV_FONT_DEFAULT=dg_font_cn_16,lvgl 核心引用该符号
target_link_libraries(test_touch_evdev PRIVATE lvgl dg_ui dg_net dg_log)
add_test(NAME test_touch_evdev COMMAND test_touch_evdev)
set_tests_properties(test_touch_evdev PROPERTIES TIMEOUT 60)

# 继电器模块(A4:宿主降级路径——装配/脉冲/参数边界/复位)
dg_add_test(test_relay dg_relay)

# 公共时基(C2:单调性/两种时钟语义)
dg_add_test(test_timeutil dg_timeutil)

# 语音播报(2026-10-04:WAV/正弦纯函数 + sink 后端注入的播放器链路)
dg_add_test(test_audio dg_audio)
set_tests_properties(test_audio PROPERTIES TIMEOUT 60)

# MQTT 通道(2026-10-04:测试内最小 broker,握手/命令/上报往返)
dg_add_test(test_mqtt dg_net dg_config dg_event_bus)
set_tests_properties(test_mqtt PROPERTIES TIMEOUT 60)

# .ota 全量升级包解析/提取(头部契约/摘要复核/暂存交接)
add_executable(test_ota_package test_ota_package.c)
target_include_directories(test_ota_package PRIVATE
    ${CMAKE_SOURCE_DIR}/services/ota)
target_link_libraries(test_ota_package PRIVATE dg_net crypto pthread)
add_test(NAME test_ota_package COMMAND test_ota_package)
set_tests_properties(test_ota_package PROPERTIES TIMEOUT 60)

# MQTT OTA 升级编排(公告解析/版本比较/下载状态机;传输注入,零网络依赖)
dg_add_test(test_ota_update dg_net dg_config dg_event_bus crypto pthread)
set_tests_properties(test_ota_update PROPERTIES TIMEOUT 120)
# 远程报警上报(sink 注入;类型/字段口径/净化)
dg_add_test(test_alarm dg_alarm)
set_tests_properties(test_alarm PROPERTIES TIMEOUT 60)

# web 监控推流(2026-10-14):sim 相机全链——降采样/编码/10fps 节流/退订/
# 配置开关;发送侧(WS 二进制)不在宿主范围,板上手验
dg_add_test(test_stream dg_web_stream)
# dg_web_stream 定义在 add_subdirectory(tests) 之后,接口目录这里显式补齐
target_include_directories(test_stream PRIVATE
    ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/proto
    ${CMAKE_SOURCE_DIR}/modules ${CMAKE_SOURCE_DIR}/modules/camera
    ${CMAKE_SOURCE_DIR}/modules/jpeg ${CMAKE_SOURCE_DIR}/modules/sqlite
    ${CMAKE_SOURCE_DIR}/services ${CMAKE_SOURCE_DIR}/services/config
    ${CMAKE_SOURCE_DIR}/services/web)
set_tests_properties(test_stream PROPERTIES TIMEOUT 60)
