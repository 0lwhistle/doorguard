# WSL 交叉编译工具链文件(door-guard)
#
# 用法(WSL):
#   1. 解压 deliverables/wsl-toolchain/ 里的两个包:
#        mkdir -p ~/dg-toolchain
#        tar xzf gcc-aarch64-10.3.tar.gz   -C ~/dg-toolchain          # 得到 gcc-aarch64/
#        tar xzf doorguard-sysroot.tar.gz  -C ~/dg-toolchain          # 得到 sysroot/(即 buildroot staging)
#   2. 编译:
#        cmake -B build -DCMAKE_TOOLCHAIN_FILE=../cmake/aarch64.cmake
#        cmake --build build
#      (工具链根目录默认 ~/dg-toolchain,可用环境变量 DG_TC_ROOT 覆盖)

set(DG_TC_ROOT "$ENV{DG_TC_ROOT}" CACHE PATH "door-guard toolchain root")
if(NOT DG_TC_ROOT)
    set(DG_TC_ROOT "$ENV{HOME}/dg-toolchain")
endif()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(TC_BIN "${DG_TC_ROOT}/gcc-aarch64/bin")
set(CMAKE_C_COMPILER   "${TC_BIN}/aarch64-none-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "${TC_BIN}/aarch64-none-linux-gnu-g++")

# buildroot staging:sysroot/{usr/include, usr/lib}
set(CMAKE_SYSROOT "${DG_TC_ROOT}/sysroot")
set(CMAKE_FIND_ROOT_PATH "${DG_TC_ROOT}/sysroot")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
