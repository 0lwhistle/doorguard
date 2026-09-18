# door-guard — RK3576 K7 人脸识别门禁应用
#
# 两种构建:
#   板上/交叉: cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64.cmake   (dg-build)
#   PC 测试:   cmake -B build-tests -DDG_BUILD_TESTS=ON                    (dg-test)
#              宿主 gcc,构建 proto 组件 + tests/ 下全部用例,demo 一并冒烟
cmake_minimum_required(VERSION 3.16)
project(door-guard C)

# 未指定构建类型时用 Debug(测试与模拟器默认带 -g;零警告不受影响)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(CMAKE_C_STANDARD 11)

# 全局零警告纪律(任务清单 §2.6);组件与测试同一标准
add_compile_options(-Wall -Wextra)

# 根目录相对头文件(如 ui/theme.h)对所有子目标可见
include_directories(${CMAKE_SOURCE_DIR})

# ---- 交叉编译变量(由工具链文件或环境提供) ----
# SDK_ROOT 指向官方 SDK 根目录
set(SDK_ROOT "$ENV{SDK_ROOT}" CACHE PATH "Rockchip SDK root")

find_package(Threads REQUIRED)

# ---- 基础组件(板上与测试同一份源码) ----
# dg_log:移植组件共用的极简日志(接口见 proto/dg_log.h)
add_library(dg_log STATIC proto/dg_log.c)
target_include_directories(dg_log PUBLIC ${CMAKE_SOURCE_DIR}/proto)

# event_bus:发布订阅事件总线(UI↔服务解耦骨干,README 见 proto/event_bus/)
add_library(dg_event_bus STATIC
    proto/event_bus/event_bus.c
    proto/event_bus/event_bus_port.c
    proto/event_bus/dg_event_pool.c
)
target_include_directories(dg_event_bus PUBLIC ${CMAKE_SOURCE_DIR}/proto/event_bus)
target_link_libraries(dg_event_bus PUBLIC dg_log)

# tasker:短任务调度器(≤500ms 约定,README 见 proto/tasker/)
add_library(dg_tasker STATIC
    proto/tasker/tasker.c
    proto/tasker/task_manager.c
    proto/tasker/task_worker.c
    proto/tasker/tasker_port.c
)
target_include_directories(dg_tasker PUBLIC ${CMAKE_SOURCE_DIR}/proto/tasker)
target_link_libraries(dg_tasker PUBLIC dg_log)

# holder:全局模块注册表(启动顺序/健康状态,README 见 proto/holder/)
add_library(dg_holder STATIC
    proto/holder/holder.c
)
target_include_directories(dg_holder PUBLIC ${CMAKE_SOURCE_DIR}/proto/holder)
target_link_libraries(dg_holder PUBLIC dg_log)

# proto 契约层:错误码/数据结构/业务事件(types.h err.h events.h,头在 proto/ 根)
add_library(dg_proto STATIC
    proto/types.c
    proto/events.c
)
target_include_directories(dg_proto PUBLIC ${CMAKE_SOURCE_DIR}/proto)
target_link_libraries(dg_proto PUBLIC dg_event_bus)

# cjson:第三方 JSON 解析(MIT,vendored 于 third_party/cjson)
add_library(cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(cjson PUBLIC ${CMAKE_SOURCE_DIR}/third_party/cjson)
# 屏蔽第三方源码的告警(不因 vendored 代码破"零警告"纪律,不修改其内容)
target_compile_options(cjson PRIVATE -w)

# config:配置体系(默认→device.json→DB device_config)
add_library(dg_config STATIC config/cfg.c)
target_include_directories(dg_config PUBLIC ${CMAKE_SOURCE_DIR}/config)
target_link_libraries(dg_config PUBLIC dg_storage cjson)

# storage:SQLite 存储加加密(storage.h API;sysroot 实测有 sqlite3/openssl3)
add_library(dg_storage STATIC
    hal/storage/storage.c
    hal/storage/crypto.c
)
target_include_directories(dg_storage PUBLIC ${CMAKE_SOURCE_DIR}/hal/storage)
target_link_libraries(dg_storage PUBLIC dg_proto sqlite3 crypto)

# ---- LVGL 8.3(third_party 源码双端同源编译:板上不再依赖 sysroot 的
# liblvgl.so,以获得自定义 lv_conf:CJK 字体/256KB 内存池) ----
file(GLOB_RECURSE LVGL_SRCS third_party/lvgl/src/*.c)
add_library(lvgl STATIC ${LVGL_SRCS})
target_include_directories(lvgl PUBLIC ${CMAKE_SOURCE_DIR}/third_party/lvgl)
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_compile_options(lvgl PRIVATE -w)   # vendored 源码不改动内容,告警豁免

# ---- UI 子系统(theme/i18n/widgets/page_mgr/字体) ----
file(GLOB DG_WIDGET_SRCS ui/widgets/*.c)
add_library(dg_ui STATIC
    ui/ui.c
    ui/port.c
    ui/i18n.c
    ui/navigator/navigator.c
    ui/bridge/bridge.c
    ui/pages/page_home.c
    ui/pages/page_standby.c
    ui/pages/page_menu.c
    ui/pages/page_users.c
    ui/pages/page_device.c
    ui/pages/page_access_set.c
    ui/pages/page_logs.c
    ui/presenters/presenter_home.c
    ui/presenters/presenter_standby.c
    ui/presenters/presenter_menu.c
    ui/presenters/presenter_users.c
    ui/presenters/presenter_device.c
    ui/presenters/presenter_access_set.c
    ui/presenters/presenter_logs.c
    ui/ui_events.c
    ui/font/dg_font_cn_16.c
    ${DG_WIDGET_SRCS}
)
target_include_directories(dg_ui PUBLIC ${CMAKE_SOURCE_DIR}/ui ${CMAKE_SOURCE_DIR}/modules/access)
target_link_libraries(dg_ui PUBLIC lvgl dg_config dg_access)

# access:验证状态机 + 认证融合服务(FSM 持有者,日志/开门唯一出口)
add_library(dg_access STATIC
    modules/access/auth_fsm.c
    modules/access/access_service.c
)
target_include_directories(dg_access PUBLIC ${CMAKE_SOURCE_DIR}/modules/access)
target_link_libraries(dg_access PUBLIC dg_proto dg_config dg_tasker dg_gpio)

# vision:视觉服务(槽位句柄 + sim mock / rockiva 占位)
add_library(dg_vision STATIC modules/vision/vision_service.c)
target_include_directories(dg_vision PUBLIC ${CMAKE_SOURCE_DIR}/modules/vision)
target_link_libraries(dg_vision PUBLIC dg_proto)
if(DG_SIM)
    target_sources(dg_vision PRIVATE modules/vision/vision_sim.c)
else()
    target_sources(dg_vision PRIVATE modules/vision/vision_rockiva.c)
endif()

# enroll:录入编排
add_library(dg_enroll STATIC modules/enroll/enroll_service.c)
target_include_directories(dg_enroll PUBLIC ${CMAKE_SOURCE_DIR}/modules/enroll)
target_link_libraries(dg_enroll PUBLIC dg_proto dg_storage dg_vision)

# capture:取流状态服务
add_library(dg_capture STATIC modules/capture/capture_service.c)
target_include_directories(dg_capture PUBLIC ${CMAKE_SOURCE_DIR}/modules/capture)
target_link_libraries(dg_capture PUBLIC dg_proto dg_tasker)

# gpio:门控(sysfs;rootfs 带 libgpiod 后迁移)
add_library(dg_gpio STATIC hal/gpio/gpio_hal.c)
target_include_directories(dg_gpio PUBLIC ${CMAKE_SOURCE_DIR}/hal/gpio)
target_link_libraries(dg_gpio PUBLIC dg_log)

# uart:串口框架(mock 回环 / termios)
add_library(dg_uart STATIC hal/uart/uart_hal.c)
target_include_directories(dg_uart PUBLIC ${CMAKE_SOURCE_DIR}/hal/uart)
target_link_libraries(dg_uart PUBLIC dg_log)

# liveness:活体占位
add_library(dg_liveness STATIC modules/liveness/liveness_service.c)
target_include_directories(dg_liveness PUBLIC ${CMAKE_SOURCE_DIR}/modules/liveness)
target_link_libraries(dg_liveness PUBLIC dg_proto)

# civetweb:第三方 HTTP/WebSocket 单库(MIT,vendored)
add_library(civetweb STATIC third_party/civetweb/civetweb.c)
target_include_directories(civetweb PUBLIC third_party/civetweb)
target_include_directories(civetweb PRIVATE ${CMAKE_SOURCE_DIR}/third_party/civetweb)
target_compile_options(civetweb PRIVATE -w)
target_compile_definitions(civetweb PRIVATE
    MG_ENABLE_IPV6=0 MG_ENABLE_SSL=0 MG_DISABLE_POPEN=1
    NO_SSL=1 NO_SSL_DL=1 MG_ENABLE_MD5=1 USE_WEBSOCKET=1
    MG_ENABLE_SHA1=1 OPENSSL_API_3_0=1)
target_compile_definitions(civetweb PUBLIC USE_WEBSOCKET=1)

# net:web 上位机 / OTA / NTP / mDNS
add_library(dg_net STATIC
    modules/net/web/web_server.c
    modules/net/web/web_pages.c
    modules/net/ota/ota_service.c
    modules/net/ntp/ntp_service.c
    modules/net/mdns/mdns_responder.c
)
target_include_directories(dg_net PUBLIC ${CMAKE_SOURCE_DIR}/modules/net)
target_link_libraries(dg_net PUBLIC dg_access dg_storage civetweb crypto)

if(DG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    return()
endif()

# ---- 显示/相机 HAL(按端选择 sim 后端;#ifdef 只出现在后端与 main) ----
if(DG_SIM)
    find_package(SDL2 REQUIRED)
    add_library(dg_display STATIC hal/display/display_sim.c)
    target_link_libraries(dg_display PUBLIC lvgl SDL2::SDL2)
    add_library(dg_camera STATIC hal/camera/camera_sim.c)
    target_include_directories(dg_camera PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb)
    target_link_libraries(dg_camera PUBLIC m)   # stb_image 解码用到 pow
else()
    add_library(dg_display STATIC hal/display/display_drm.c hal/display/touch_evdev.c
                                  third_party/lv_drivers/drm.c)
    # 顺序敏感:libdrm 在前——lv_drivers/drm.c 内部 #include <drm.h> 必须解析到
    # libdrm 的 drm.h 而不是本目录同名头(避免 xf86drm 类型未定义)
    target_include_directories(dg_display PRIVATE ${CMAKE_SYSROOT}/usr/include/libdrm)
    target_include_directories(dg_display PRIVATE ${CMAKE_SOURCE_DIR}/third_party/lv_drivers)
    set_source_files_properties(third_party/lv_drivers/drm.c PROPERTIES COMPILE_OPTIONS "-w")
    target_link_libraries(dg_display PUBLIC lvgl drm)   # libdrm:sysroot 实测有
    add_library(dg_camera STATIC hal/camera/camera_board.c)
    target_include_directories(dg_camera PRIVATE
        ${CMAKE_SYSROOT}/usr/include/rkaiq
        ${CMAKE_SYSROOT}/usr/include/rga)
    # rkaiq:3A 经板上 rkaiq_3A_server;librga:NV12→XRGB 硬件旋转转换
    target_link_libraries(dg_camera PUBLIC rkaiq rga)
endif()
target_link_libraries(dg_display PUBLIC dg_log)
target_link_libraries(dg_camera PUBLIC dg_log)

# ---- 应用(交叉编译产物,dg-deploy 推板) ----
include_directories(
    ${CMAKE_SOURCE_DIR}
    # ROCKIVA(官方人脸方案,B7 阶段启用)
    ${SDK_ROOT}/external/iva/librockiva/rockiva-rk3576-Linux/include
    # LVGL
    # ${SDK_ROOT}/app/lvgl_demo/lvgl
)
# 库路径(启用 rockiva 时打开)
link_directories(
    ${SDK_ROOT}/external/iva/librockiva/rockiva-rk3576-Linux/lib64
)

add_executable(door-guard app/main.c)
# 组件链入应用:装配顺序在 app/main.c(architecture.md §1,main.c 头注同步维护)。
# --group:lv_conf 时基(LV_TICK_CUSTOM)回引 ui/port.c 而部分 lvgl 成员又被 ui
# 引用,静态库符号环需组解析
target_link_libraries(door-guard PRIVATE
    -Wl,--start-group
    dg_event_bus dg_tasker dg_holder dg_proto dg_storage dg_config
    lvgl dg_ui dg_access dg_vision dg_enroll dg_capture dg_liveness dg_gpio dg_uart dg_net dg_display dg_camera
    -Wl,--end-group)

if(DG_SIM)
    target_compile_definitions(door-guard PRIVATE DG_SIM=1)
endif()
# rockiva     # 官方人脸库(B7 阶段启用)
# sqlite3     # 存储(启用 storage HAL 时)
# gpiod       # 门控 GPIO
