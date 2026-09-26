# door-guard — RK3576 K7 人脸识别门禁应用
#
# 两种构建:
#   板上/交叉: cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64.cmake   (dg-build)
#   PC 测试:   cmake -B build-tests -DDG_BUILD_TESTS=ON                    (dg-test)
#              宿主 gcc,构建 components+proto 组件 + tests/ 下全部用例,demo 一并冒烟
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

find_package(Threads REQUIRED)

# jpeg:头像编解码(modules/jpeg)。sysroot 里混进了两套 libjpeg:dev 名
# libjpeg.so 实为旧 IJG 6b ABI(SONAME .62),板上运行库只有 turbo 的
# libjpeg.so.8——必须显式链 .8,否则板上加载失败(rc=127,2026-09-21 实测)。
if(CMAKE_SYSROOT AND EXISTS "${CMAKE_SYSROOT}/usr/lib/libjpeg.so.8")
    set(DG_JPEG_LIB "${CMAKE_SYSROOT}/usr/lib/libjpeg.so.8")
elseif(CMAKE_SYSROOT AND EXISTS "${CMAKE_SYSROOT}/usr/lib64/libjpeg.so.8")
    set(DG_JPEG_LIB "${CMAKE_SYSROOT}/usr/lib64/libjpeg.so.8")
else()
    find_package(JPEG REQUIRED)   # 宿主测试构建:发行版 libjpeg-turbo(同为 .8)
    set(DG_JPEG_LIB JPEG::JPEG)
endif()

# ---- 基础组件(板上与测试同一份源码) ----
# dg_log:移植组件共用的极简日志(接口见 components/logger/dg_log.h)
add_library(dg_log STATIC components/logger/dg_log.c)
target_include_directories(dg_log PUBLIC ${CMAKE_SOURCE_DIR}/components/logger)

# event_bus:发布订阅事件总线(UI↔服务解耦骨干,README 见 components/event_bus/)
add_library(dg_event_bus STATIC
    components/event_bus/event_bus.c
    components/event_bus/event_bus_port.c
    components/event_bus/dg_event_pool.c
)
target_include_directories(dg_event_bus PUBLIC ${CMAKE_SOURCE_DIR}/components/event_bus)
target_link_libraries(dg_event_bus PUBLIC dg_log)

# tasker:短任务调度器(≤500ms 约定,README 见 components/tasker/)
add_library(dg_tasker STATIC
    components/tasker/tasker.c
    components/tasker/task_manager.c
    components/tasker/task_worker.c
    components/tasker/tasker_port.c
)
target_include_directories(dg_tasker PUBLIC ${CMAKE_SOURCE_DIR}/components/tasker)
target_link_libraries(dg_tasker PUBLIC dg_log)

# holder:全局模块注册表(启动顺序/健康状态,README 见 components/holder/)
add_library(dg_holder STATIC
    components/holder/holder.c
)
target_include_directories(dg_holder PUBLIC ${CMAKE_SOURCE_DIR}/components/holder)
target_link_libraries(dg_holder PUBLIC dg_log)

# registry:services 注册表(与 holder 同构 + 心跳/重启原语;README 见 components/registry/)
add_library(dg_registry STATIC components/registry/registry.c)
target_include_directories(dg_registry PUBLIC ${CMAKE_SOURCE_DIR}/components/registry)
target_link_libraries(dg_registry PUBLIC dg_log)

# proto 契约层:错误码/数据结构/业务事件/字段合法性(头在 proto/ 根)
add_library(dg_proto STATIC
    proto/types.c
    proto/events.c
    proto/valid.c
)
target_include_directories(dg_proto PUBLIC ${CMAKE_SOURCE_DIR}/proto)
target_link_libraries(dg_proto PUBLIC dg_event_bus)

# cjson:第三方 JSON 解析(MIT,vendored 于 third_party/cjson)
add_library(cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(cjson PUBLIC ${CMAKE_SOURCE_DIR}/third_party/cjson)
# 屏蔽第三方源码的告警(不因 vendored 代码破"零警告"纪律,不修改其内容)
target_compile_options(cjson PRIVATE -w)

# config:配置服务(default 模板 + cur 原子落盘 + DB 首启迁移;M2① 双文件)
add_library(dg_config STATIC services/config/cfg.c)
target_include_directories(dg_config PUBLIC ${CMAKE_SOURCE_DIR}/services/config)
target_link_libraries(dg_config PUBLIC dg_storage cjson dg_tasker)

# storage:SQLite 存储加加密(storage.h API;sysroot 实测有 sqlite3/openssl3)
add_library(dg_storage STATIC
    modules/sqlite/storage.c
    modules/sqlite/crypto.c
)
target_include_directories(dg_storage PUBLIC ${CMAKE_SOURCE_DIR}/modules/sqlite)
target_link_libraries(dg_storage PUBLIC dg_proto sqlite3 crypto)

# jpeg:头像 JPEG 编解码(内存↔内存,libjpeg 薄封装;视觉编码/UI 解码共用)
add_library(dg_jpeg STATIC modules/jpeg/dg_jpeg.c)
target_include_directories(dg_jpeg PUBLIC ${CMAKE_SOURCE_DIR}/modules/jpeg)
target_link_libraries(dg_jpeg PUBLIC dg_proto ${DG_JPEG_LIB})

# ---- LVGL(两代 vendored 源码按 DG_USE_LVGL9 二选一,同一 `lvgl` 目标名,
# 上层链接点零改动)。OFF = 完整回退 8.3 旧栈(运行期回退通道,必须常绿)。
# C1 定版:默认 ON(契约 §2);显式 -DDG_USE_LVGL9=OFF 即回退 ----
option(DG_USE_LVGL9 "Use LVGL 9.5 (third_party/lvgl9) instead of 8.3" ON)
option(DG_LVGL_NO_NEON "Force LVGL SW draw C path (NEON crash triage)" OFF)
if(DG_USE_LVGL9)
    file(GLOB_RECURSE LVGL_SRCS third_party/lvgl9/src/*.c)
    add_library(lvgl STATIC ${LVGL_SRCS})
    # include 根 = third_party/lvgl9:`#include "lvgl.h"` 路径与 8.3 一致,
    # ui 层源码零 include 改动;lv_conf9 就在此根下(LV_CONF_INCLUDE_SIMPLE)
    target_include_directories(lvgl PUBLIC ${CMAKE_SOURCE_DIR}/third_party/lvgl9)
    target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
    if(DG_LVGL_NO_NEON)
        target_compile_definitions(lvgl PUBLIC DG_LVGL_NO_NEON)
    endif()
    target_compile_options(lvgl PRIVATE -w)   # vendored 源码不改动内容,告警豁免
    # v9 内置 Linux DRM 驱动(LV_USE_LINUX_DRM=1)的头会经 lvgl.h 进入所有
    # 使用方:xf86drmMode.h 内部 #include <drm.h>,须再给 libdrm 子目录路径。
    # 交叉取 sysroot,宿主取发行版 libdrm-dev;PUBLIC 保证每个含 lvgl.h 的
    # 编译单元都能解析(drm.h 不在默认搜索路径)
    if(CMAKE_SYSROOT AND EXISTS "${CMAKE_SYSROOT}/usr/include/libdrm/drm.h")
        target_include_directories(lvgl SYSTEM PUBLIC
            ${CMAKE_SYSROOT}/usr/include/libdrm)
    elseif(EXISTS "/usr/include/libdrm/drm.h")
        target_include_directories(lvgl SYSTEM PUBLIC /usr/include/libdrm)
    endif()
else()
    # ---- LVGL 8.3(third_party 源码双端同源编译:板上不再依赖 sysroot 的
    # liblvgl.so,以获得自定义 lv_conf:CJK 字体/256KB 内存池) ----
    file(GLOB_RECURSE LVGL_SRCS third_party/lvgl/src/*.c)
    add_library(lvgl STATIC ${LVGL_SRCS})
    target_include_directories(lvgl PUBLIC ${CMAKE_SOURCE_DIR}/third_party/lvgl)
    target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
    if(DG_LVGL_NO_NEON)
        target_compile_definitions(lvgl PUBLIC DG_LVGL_NO_NEON)
    endif()
    target_compile_options(lvgl PRIVATE -w)   # vendored 源码不改动内容,告警豁免
endif()

# ---- UI 子系统(theme/i18n/widgets/page_mgr/字体) ----
# (C2 起随 LVGL9 编译;上方守卫已于 ui/ 迁移完成时解除)
file(GLOB DG_WIDGET_SRCS ui/widgets/*.c)
add_library(dg_ui STATIC
    ui/ui.c
    ui/port.c
    ui/i18n.c
    ui/valid_ui.c
    ui/navigator/navigator.c
    ui/bridge/bridge.c
    ui/pages/page_home.c
    ui/pages/page_standby.c
    ui/pages/page_menu.c
    ui/pages/page_users.c
    ui/pages/page_user_edit.c
    ui/pages/page_capture.c
    ui/pages/page_device.c
    ui/pages/page_access_set.c
    ui/pages/page_logs.c
    ui/pages/page_web_set.c
    ui/presenters/presenter_home.c
    ui/presenters/presenter_standby.c
    ui/presenters/presenter_menu.c
    ui/presenters/presenter_users.c
    ui/presenters/presenter_user_edit.c
    ui/presenters/presenter_capture.c
    ui/presenters/presenter_device.c
    ui/presenters/presenter_access_set.c
    ui/presenters/presenter_logs.c
    ui/presenters/presenter_web_set.c
    ui/ui_events.c
    ui/font/dg_font_cn_16.c
    ${DG_WIDGET_SRCS}
)
target_include_directories(dg_ui PUBLIC ${CMAKE_SOURCE_DIR}/ui ${CMAKE_SOURCE_DIR}/services/access)
target_link_libraries(dg_ui PUBLIC lvgl dg_config dg_access dg_storage dg_jpeg
                                   dg_net_info)

# access:验证状态机 + 认证融合服务(FSM 持有者,日志/开门唯一出口)
add_library(dg_access STATIC
    services/access/auth_fsm.c
    services/access/access_service.c
)
target_include_directories(dg_access PUBLIC ${CMAKE_SOURCE_DIR}/services/access
                                            ${CMAKE_SOURCE_DIR}/services/vision)
target_link_libraries(dg_access PUBLIC dg_proto dg_config dg_tasker dg_gpio)

# vision:视觉服务(槽位/模式/后端注册表 + sim mock / rockiva 板上后端)
add_library(dg_vision STATIC services/vision/vision_service.c)
target_include_directories(dg_vision PUBLIC ${CMAKE_SOURCE_DIR}/services/vision)
# 服务层要注入 storage 查重比较器 + 登记特征口径(device_config),并读 cfg
target_link_libraries(dg_vision PUBLIC dg_proto dg_storage dg_config dg_jpeg)
# 测试构建走 sim 后端:测试是宿主 gcc 无 rockiva 头/库(板上后端只在交叉编译里编)
if(DG_SIM OR DG_BUILD_TESTS)
    target_sources(dg_vision PRIVATE services/vision/vision_sim.c)
else()
    target_sources(dg_vision PRIVATE services/vision/vision_rockiva.c
                                     services/vision/vision_rknn.c)
    target_include_directories(dg_vision PRIVATE
        ${CMAKE_SYSROOT}/usr/include/rockiva
        ${CMAKE_SOURCE_DIR}/modules/sqlite
        ${CMAKE_SOURCE_DIR}/services/config
        ${CMAKE_SOURCE_DIR}/services/liveness
        ${CMAKE_SOURCE_DIR}/drv/npu)
    # 库链接在 dg_camera 定义之后统一补(见下方"rockiva 链接"块)
endif()


# rknn 人脸后处理(SCRFD 解码/NMS/5 点对齐/特征比对)
# 纯 C、零硬件依赖 → 宿主与板上都编:这是"能在宿主验证"的部分,
# 硬件侧(喂帧/推理)在 drv/npu,契约装配在 vision_rknn.c。
add_library(dg_rknn_face STATIC services/vision/rknn_face.c
                                services/vision/face_quality.c)
target_include_directories(dg_rknn_face PUBLIC ${CMAKE_SOURCE_DIR}/services/vision)
target_link_libraries(dg_rknn_face PUBLIC m)

# enroll:录入编排
add_library(dg_enroll STATIC services/enroll/enroll_service.c)
target_include_directories(dg_enroll PUBLIC ${CMAKE_SOURCE_DIR}/services/enroll)
target_link_libraries(dg_enroll PUBLIC dg_proto dg_storage dg_vision)

# capture:取流状态服务
add_library(dg_capture STATIC services/capture/capture_service.c)
target_include_directories(dg_capture PUBLIC ${CMAKE_SOURCE_DIR}/services/capture)
target_link_libraries(dg_capture PUBLIC dg_proto dg_tasker)

# gpio:门控(sysfs;rootfs 带 libgpiod 后迁移)
add_library(dg_gpio STATIC drv/gpio/gpio_hal.c)
target_include_directories(dg_gpio PUBLIC ${CMAKE_SOURCE_DIR}/drv/gpio
                                          ${CMAKE_SOURCE_DIR}/proto)
target_link_libraries(dg_gpio PUBLIC dg_log)

# uart:串口框架(mock 回环 / termios)
add_library(dg_uart STATIC drv/uart/uart_hal.c)
target_include_directories(dg_uart PUBLIC ${CMAKE_SOURCE_DIR}/drv/uart
                                          ${CMAKE_SOURCE_DIR}/proto)
target_link_libraries(dg_uart PUBLIC dg_log)

# npu:NPU 推理库(RKNN 运行时薄封装)
# 宿主没有 rknn 头/驱动,只在交叉编译里编(与 vision_rockiva.c 同一条守卫);
# 全仓只有 drv/npu/npu_model.c include <rknn_api.h>,上层只认 npu_model.h。
if(NOT DG_SIM AND NOT DG_BUILD_TESTS)
    add_library(dg_npu STATIC drv/npu/npu_model.c drv/npu/npu_pre.c)
    target_include_directories(dg_npu PUBLIC ${CMAKE_SOURCE_DIR}/drv/npu
                                             ${CMAKE_SOURCE_DIR}/proto)
    target_link_libraries(dg_npu PUBLIC dg_log rknnrt rga)
    target_include_directories(dg_npu PRIVATE ${CMAKE_SYSROOT}/usr/include/rknn
                                              ${CMAKE_SYSROOT}/usr/include/rga)

    # NPU 模型探针(板上跑,不进 door-guard 主程序):打印 .rknn 的输入输出规格
    # 并零输入试跑。换模型第一件事——解码要按真实张量形状写,不能猜。
    add_executable(npu_probe tools/npu_probe.c)
    target_link_libraries(npu_probe PRIVATE dg_npu)

    # 检测链路离线对拍(板上跑):喂一张已 letterbox 好的原始 RGB 走完整
    # "推理+解码",拿已知人脸位置的标准图验证链路,不必有人站镜头前。
    add_executable(rknn_det_test tools/rknn_det_test.c)
    target_link_libraries(rknn_det_test PRIVATE dg_npu dg_rknn_face)

    # 特征链路验证(板上跑):三张 112×112(同人正常/变暗/纯色)算两两余弦,
    # 验证 ArcFace 的输入归一化假设——错了这里立刻暴露,不会带到产品路径。
    add_executable(rknn_rec_test tools/rknn_rec_test.c)
    target_link_libraries(rknn_rec_test PRIVATE dg_npu dg_rknn_face)
endif()

# liveness:动作活体(B7 留口 on_face/pass;B8 状态机)
add_library(dg_liveness STATIC services/liveness/liveness_service.c)
target_include_directories(dg_liveness PUBLIC ${CMAKE_SOURCE_DIR}/services/liveness)
target_link_libraries(dg_liveness PUBLIC dg_proto dg_config)

# mongoose:第三方网络库(GPLv2/商业双许可,单文件 vendored;本项目按 GPLv2 开源路线使用)
# 统一网络层的唯一传输引擎(web/OTA/NTP/mDNS 全走 modules/net 的 netcore 事件循环;
# 2026-09-22 起 civetweb 退役——两库 mg_* 符号同名,本就不能共存于一个二进制)
add_library(mongoose STATIC third_party/mongoose/mongoose.c)
target_include_directories(mongoose PUBLIC third_party/mongoose)
target_compile_definitions(mongoose PRIVATE MG_ARCH=MG_ARCH_UNIX MG_ENABLE_TCPIP=0)

# net:web 上位机 / OTA / NTP / mDNS(源码分驻 services/{web,ota,ntp,mdns} 与 modules/net)
# net_info:网络接口只读信息(独立小库:web 上位机与 UI 主页网络图标共用,
# UI 不能整链 dg_net——那会把 web/ota/mongoose 全拖进来)
add_library(dg_net_info STATIC modules/net/net_info.c)
target_include_directories(dg_net_info PUBLIC ${CMAKE_SOURCE_DIR}/modules/net
                                              ${CMAKE_SOURCE_DIR}/proto)
target_link_libraries(dg_net_info PUBLIC dg_log)

add_library(dg_net STATIC
    modules/net/netcore.c
    services/web/web_server.c
    services/web/web_pages.c
    services/web/web_auth.c
    services/web/web_session.c
    services/ota/ota_service.c
    services/ntp/ntp_service.c
    services/mdns/mdns_responder.c
    services/mdns/mdns_wire.c
)
target_include_directories(dg_net PUBLIC ${CMAKE_SOURCE_DIR}/services
                                         ${CMAKE_SOURCE_DIR}/modules)
target_link_libraries(dg_net PUBLIC dg_net_info)
# crypto:web_auth(PBKDF2)/web_session(RAND)依赖;mongoose 自带 SHA1 无需 shim
target_link_libraries(dg_net PUBLIC dg_access dg_storage mongoose crypto)

# 固件版本号:git describe 注入(上位机"设备概览"与 /api/device 显示)。
# 无 git / 无 tag 时退化为 unknown——不能因为版本号取不到就让编译失败。
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE DG_GIT_DESC
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT DG_GIT_DESC)
    set(DG_GIT_DESC "unknown")
endif()
target_compile_definitions(dg_net PUBLIC DG_FW_VERSION="${DG_GIT_DESC}")

if(DG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    return()
endif()

# ---- 显示/相机模块(按端与 DG_USE_LVGL9 选择后端;#ifdef 只出现在后端与 main) ----
if(DG_SIM)
    find_package(SDL2 REQUIRED)
    if(DG_USE_LVGL9)
        add_library(dg_display STATIC modules/display/display_sim_v9.c
                                      modules/display/touch_activity.c)
        target_compile_definitions(dg_display PRIVATE DG_USE_LVGL9=1)
    else()
        add_library(dg_display STATIC modules/display/display_sim.c
                                      modules/display/touch_activity.c)
    endif()
    target_link_libraries(dg_display PUBLIC lvgl SDL2::SDL2)
    add_library(dg_camera STATIC modules/camera/camera_sim.c)
    target_include_directories(dg_camera PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb)
    target_link_libraries(dg_camera PUBLIC m)   # stb_image 解码用到 pow
else()
    if(DG_USE_LVGL9)
        # v9:内置 Linux DRM 驱动(dumb x2 + atomic 翻转)在 lvgl 库内编译,
        # 这里只留薄封装 + 自研触摸;lv_drivers 不再参与
        add_library(dg_display STATIC modules/display/display_drm_v9.c
                                      modules/display/touch_evdev.c
                                      modules/display/touch_activity.c)
        target_compile_definitions(dg_display PRIVATE DG_USE_LVGL9=1)
    else()
        add_library(dg_display STATIC modules/display/display_drm.c
                                      modules/display/touch_evdev.c
                                      modules/display/touch_activity.c
                                      third_party/lv_drivers/drm.c)
        # 顺序敏感:libdrm 在前——lv_drivers/drm.c 内部 #include <drm.h> 必须解析到
        # libdrm 的 drm.h 而不是本目录同名头(避免 xf86drm 类型未定义)
        target_include_directories(dg_display PRIVATE ${CMAKE_SYSROOT}/usr/include/libdrm)
        target_include_directories(dg_display PRIVATE ${CMAKE_SOURCE_DIR}/third_party/lv_drivers)
        set_source_files_properties(third_party/lv_drivers/drm.c PROPERTIES COMPILE_OPTIONS "-w")
    endif()
    target_link_libraries(dg_display PUBLIC lvgl drm)   # libdrm:sysroot 实测有
    add_library(dg_camera STATIC modules/camera/camera_board.c)
    target_include_directories(dg_camera PRIVATE
        ${CMAKE_SYSROOT}/usr/include/rkaiq
        ${CMAKE_SYSROOT}/usr/include/rga)
    # rkaiq:3A 经板上 rkaiq_3A_server;librga:NV12→XRGB 硬件旋转转换
    target_link_libraries(dg_camera PUBLIC rkaiq rga)
endif()

# LVGL9 最小渲染验证入口(C1 门禁工具):不依赖 ui 层,sim(SDL)与交叉(DRM)
# 同一份 main;自动取证见 lvgl9_smoke.c 头注(DG_SMOKE_FRAMES/DG_SMOKE_DUMP_BMP)
if(DG_USE_LVGL9)
    add_executable(dg_lvgl9_smoke modules/display/lvgl9_smoke.c)
    target_link_libraries(dg_lvgl9_smoke PRIVATE dg_display)
endif()
target_link_libraries(dg_display PUBLIC dg_log)
target_link_libraries(dg_camera PUBLIC dg_log)
# 两个模块的头直接引用 proto/err.h —— 显式声明对 proto 契约的依赖
# (原先靠 dg_log 的 include 目录恰好是 proto/ 而意外传导,迁移后必须写明)
target_include_directories(dg_display PUBLIC ${CMAKE_SOURCE_DIR}/proto)
target_include_directories(dg_camera PUBLIC ${CMAKE_SOURCE_DIR}/proto)

# rockiva 视觉链接(dg_vision 引用 camera/storage/cfg/liveness 符号;
# 必须在这些库定义之后,CMake 才能建立正确的静态链接顺序)
if(NOT DG_SIM)
    target_link_libraries(dg_vision PUBLIC rockiva dg_storage dg_config dg_camera
                                           dg_liveness dg_npu dg_rknn_face)
endif()

# ---- 应用(交叉编译产物,dg-deploy 推板) ----
# ROCKIVA 的头与库都取自 sysroot(与板上 .so 同源):
#   ${CMAKE_SYSROOT}/usr/include/rockiva、${CMAKE_SYSROOT}/usr/lib/librockiva.so
# 曾用 SDK_ROOT/external/iva/... 的 include/lib64 路径——SDK_ROOT 为空时会在
# 可执行文件里烧进 RPATH=/external/iva/...(板上实测 ENOENT 但无害);已移除。
include_directories(${CMAKE_SOURCE_DIR})

add_executable(door-guard app/main.c)
# 组件链入应用:装配顺序在 app/main.c(architecture.md §1,main.c 头注同步维护)。
# --group:lv_conf 时基(LV_TICK_CUSTOM)回引 ui/port.c 而部分 lvgl 成员又被 ui
# 引用,静态库符号环需组解析
target_link_libraries(door-guard PRIVATE
    -Wl,--start-group
    dg_event_bus dg_tasker dg_holder dg_registry dg_proto dg_storage dg_config
    lvgl dg_ui dg_access dg_vision dg_enroll dg_capture dg_liveness dg_gpio dg_uart dg_net dg_display dg_camera
    -Wl,--end-group)

if(DG_SIM)
    target_compile_definitions(door-guard PRIVATE DG_SIM=1)
endif()
# rockiva     # 官方人脸库(B7 阶段启用)
# sqlite3     # 存储(启用 storage HAL 时)
# gpiod       # 门控 GPIO
