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
dg_add_test(test_uart_mock dg_uart)
set_tests_properties(test_auth_fsm PROPERTIES TIMEOUT 60)
dg_add_test(test_rknn_face dg_rknn_face)
dg_add_test(test_face_quality dg_rknn_face)
# 头像 JPEG 编解码(往返保真/缩放解码/损坏输入不崩)
dg_add_test(test_jpeg dg_jpeg)
# letterbox 是头内联的纯数学,测试只需那个头目录(不需要 dg_npu:它含 RGA,宿主编不了)
dg_add_test(test_npu_pre dg_rknn_face)

# 录入链路端到端(sim mock 后端应答抓取):请求→回执→DB 状态一致
dg_add_test(test_enroll_flow dg_enroll dg_vision dg_storage dg_config
                             dg_tasker dg_event_bus dg_proto cjson)
set_tests_properties(test_enroll_flow PROPERTIES TIMEOUT 120)
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
target_link_libraries(test_web_auth PRIVATE dg_storage dg_proto crypto pthread)
add_test(NAME test_web_auth COMMAND test_web_auth)
set_tests_properties(test_web_auth PROPERTIES TIMEOUT 60)

# netcore:统一网络事件循环(启停/心跳/post 闭包投递;不建真实连接)
# netcore 与 mongoose 直连被测源文件(不整链 dg_net,保持单测最小依赖)
add_executable(test_netcore test_netcore.c
    ${CMAKE_SOURCE_DIR}/modules/net/netcore.c)
target_include_directories(test_netcore PRIVATE
    ${CMAKE_SOURCE_DIR}/modules/net ${CMAKE_SOURCE_DIR}/third_party/mongoose
    ${CMAKE_SOURCE_DIR}/proto ${CMAKE_SOURCE_DIR}/components/logger)
target_link_libraries(test_netcore PRIVATE mongoose dg_log pthread)
add_test(NAME test_netcore COMMAND test_netcore)
set_tests_properties(test_netcore PROPERTIES TIMEOUT 30)

# OTA 按需写线程流水线(环形缓冲/校验闭环/断点续传/进度事件;DG_OTA_DIR=临时目录)
add_executable(test_ota test_ota.c)
target_include_directories(test_ota PRIVATE
    ${CMAKE_SOURCE_DIR}/services/ota ${CMAKE_SOURCE_DIR}/proto
    ${CMAKE_SOURCE_DIR}/components/event_bus)
target_link_libraries(test_ota PRIVATE dg_net crypto pthread)
add_test(NAME test_ota COMMAND test_ota)
set_tests_properties(test_ota PROPERTIES TIMEOUT 120)

# i18n 一致性:扫描源码 _() 键 + 双 json 覆盖 + 裸中文检查 + 字体字形覆盖
# (C3:随 LVGL9 编译,lv_font_get_glyph_dsc 两代同签名;头路径按代际切换)
add_executable(test_i18n test_i18n.c)
target_compile_definitions(test_i18n PRIVATE DG_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(DG_USE_LVGL9)
    target_include_directories(test_i18n PRIVATE ${CMAKE_SOURCE_DIR}/third_party/lvgl9)
else()
    target_include_directories(test_i18n PRIVATE ${CMAKE_SOURCE_DIR}/third_party/lvgl)
endif()
target_link_libraries(test_i18n PRIVATE lvgl dg_ui cjson)
add_test(NAME test_i18n COMMAND test_i18n)
set_tests_properties(test_i18n PROPERTIES TIMEOUT 60)

# widget 冒烟:无头 LVGL 下真实 widget 创建/交互/弹窗生命周期
add_executable(test_widgets test_widgets.c)
target_compile_definitions(test_widgets PRIVATE DG_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_widgets PRIVATE lvgl dg_ui)
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
# 被测源 touch_evdev.c 含 v8/v9 双栈注册(#ifdef DG_USE_LVGL9),按构建代际
# 切头文件路径与编译定义;测试内容两个代际完全同一份
add_executable(test_touch_evdev test_touch_evdev.c
               ${CMAKE_SOURCE_DIR}/modules/display/touch_evdev.c
               ${CMAKE_SOURCE_DIR}/modules/display/touch_activity.c)
if(DG_USE_LVGL9)
    target_compile_definitions(test_touch_evdev PRIVATE DG_USE_LVGL9=1)
    target_include_directories(test_touch_evdev PRIVATE
        ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/proto
        ${CMAKE_SOURCE_DIR}/third_party/lvgl9)
    # dg_ui:lv_conf9 的 LV_FONT_DEFAULT=dg_font_cn_16,lvgl 核心引用该符号
    target_link_libraries(test_touch_evdev PRIVATE lvgl dg_ui dg_log)
else()
    target_include_directories(test_touch_evdev PRIVATE
        ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/proto
        ${CMAKE_SOURCE_DIR}/third_party/lvgl)
    target_link_libraries(test_touch_evdev PRIVATE lvgl dg_ui dg_log)
endif()
add_test(NAME test_touch_evdev COMMAND test_touch_evdev)
set_tests_properties(test_touch_evdev PROPERTIES TIMEOUT 60)
