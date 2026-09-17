# door-guard — RK3576 K7 人脸识别门禁应用
#
# 两种构建:
#   板上/交叉: cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64.cmake   (dg-build)
#   PC 测试:   cmake -B build-tests -DDG_BUILD_TESTS=ON                    (dg-test)
#              宿主 gcc,构建 proto 组件 + tests/ 下全部用例,demo 一并冒烟
cmake_minimum_required(VERSION 3.16)
project(door-guard C)

set(CMAKE_C_STANDARD 11)

# 全局零警告纪律(任务清单 §2.6);组件与测试同一标准
add_compile_options(-Wall -Wextra)

# ---- 交叉编译变量(由工具链文件或环境提供) ----
# SDK_ROOT 指向官方 SDK 根目录
set(SDK_ROOT "$ENV{SDK_ROOT}" CACHE PATH "Rockchip SDK root")

find_package(Threads REQUIRED)

# ---- 基础组件(板上与测试同一份源码) ----
# dg_log:移植组件共用的极简日志(接口见 proto/dg_log.h)
add_library(dg_log STATIC proto/dg_log.c)
target_include_directories(dg_log PUBLIC proto)

# event_bus:发布订阅事件总线(UI↔服务解耦骨干,README 见 proto/event_bus/)
add_library(dg_event_bus STATIC
    proto/event_bus/event_bus.c
    proto/event_bus/event_bus_port.c
    proto/event_bus/dg_event_pool.c
)
target_include_directories(dg_event_bus PUBLIC proto/event_bus)
target_link_libraries(dg_event_bus PUBLIC dg_log)

# tasker:短任务调度器(≤500ms 约定,README 见 proto/tasker/)
add_library(dg_tasker STATIC
    proto/tasker/tasker.c
    proto/tasker/task_manager.c
    proto/tasker/task_worker.c
    proto/tasker/tasker_port.c
)
target_include_directories(dg_tasker PUBLIC proto/tasker)
target_link_libraries(dg_tasker PUBLIC dg_log)

# holder:全局模块注册表(启动顺序/健康状态,README 见 proto/holder/)
add_library(dg_holder STATIC
    proto/holder/holder.c
)
target_include_directories(dg_holder PUBLIC proto/holder)
target_link_libraries(dg_holder PUBLIC dg_log)

if(DG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    return()
endif()

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
# 组件链入应用:装配顺序在 app/main.c(architecture.md §1,main.c 头注同步维护)
target_link_libraries(door-guard PRIVATE dg_event_bus dg_tasker dg_holder)
# rockiva     # 官方人脸库(B7 阶段启用)
# sqlite3     # 存储(启用 storage HAL 时)
# gpiod       # 门控 GPIO
