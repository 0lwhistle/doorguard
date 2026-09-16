# door-guard — RK3576 K7 人脸识别门禁应用
#
# 交叉编译(在编译 VM 上):
#   source <SDK>/buildroot/output/<cfg>/host/environment-setup  # 若 buildroot 提供
#   或手动指定工具链:cmake -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64.cmake ..
cmake_minimum_required(VERSION 3.16)
project(door-guard C)

set(CMAKE_C_STANDARD 11)

# ---- 交叉编译变量(由工具链文件或环境提供) ----
# SDK_ROOT 指向官方 SDK 根目录
set(SDK_ROOT "$ENV{SDK_ROOT}" CACHE PATH "Rockchip SDK root")

# ---- 头文件路径 ----
include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/proto
    # ROCKIVA(官方人脸方案)
    ${SDK_ROOT}/external/iva/librockiva/rockiva-rk3576-Linux/include
    # LVGL
    # ${SDK_ROOT}/app/lvgl_demo/lvgl
)

# ---- 库路径 ----
link_directories(
    ${SDK_ROOT}/external/iva/librockiva/rockiva-rk3576-Linux/lib64
)

# ---- 模块源文件(骨架期逐模块充实) ----
file(GLOB PROTO_SRCS  proto/*.c)
file(GLOB APP_SRCS    app/*.c)

add_executable(door-guard
    ${APP_SRCS}
    ${PROTO_SRCS}
)

target_link_libraries(door-guard
    pthread
    # rockiva     # 官方人脸库(B7 阶段启用)
    # sqlite3     # 存储(启用 storage HAL 时)
    # gpiod       # 门控 GPIO
)
