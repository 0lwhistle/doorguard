# WSL 交叉编译工具链文件(door-guard)
#
# 用法(WSL):
#   1. 解压 deliverables/wsl-toolchain/ 里的 gcc 包(先 md5sum -c):
#        mkdir -p ~/dg-toolchain
#        tar xzf gcc-aarch64-10.3.tar.gz -C ~/dg-toolchain
#        # 解出 gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/(本文件自动探测)
#   2. 编译:
#        cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64.cmake
#        cmake --build build
#      (工具链根目录默认 ~/dg-toolchain,可用环境变量 DG_TC_ROOT 覆盖)

set(DG_TC_ROOT "$ENV{DG_TC_ROOT}" CACHE PATH "door-guard toolchain root")
if(NOT DG_TC_ROOT)
    # 候选位置依次探测(HOME 因执行用户而异)
    foreach(_c "$ENV{HOME}/dg-toolchain" "/home/olwhistle/dg-toolchain")
        if(EXISTS "${_c}/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin")
            set(DG_TC_ROOT "${_c}")
            break()
        endif()
    endforeach()
endif()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# gcc 包顶层目录名带版本号,自动探测(兼容 gcc-aarch64 别名)
file(GLOB _GCC_DIR "${DG_TC_ROOT}/gcc-arm-*aarch64-none-linux-gnu" "${DG_TC_ROOT}/gcc-aarch64")
list(LENGTH _GCC_DIR _N)
if(_N EQUAL 0)
    message(FATAL_ERROR "${DG_TC_ROOT} 下没找到交叉编译器,先按 deliverables/wsl-toolchain/README.md 解压 gcc 包")
endif()
list(GET _GCC_DIR 0 _GCC_DIR)

set(CMAKE_C_COMPILER   "${_GCC_DIR}/bin/aarch64-none-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "${_GCC_DIR}/bin/aarch64-none-linux-gnu-g++")

# sysroot:重导出的 doorguard sysroot 优先(sysroot/ 为标准名,staging/ 兼容旧导出);
# 都没有时用编译器自带 libc —— stdio 级程序可编可跑,但 rockiva/gstreamer/sqlite 等
# 板载库要用必须先在 VM 重导 sysroot(见 deliverables/wsl-toolchain/README.md)
set(CMAKE_SYSROOT "")
if(EXISTS "${DG_TC_ROOT}/sysroot/usr/include")
    set(CMAKE_SYSROOT "${DG_TC_ROOT}/sysroot")
elseif(EXISTS "${DG_TC_ROOT}/staging/usr/include")
    set(CMAKE_SYSROOT "${DG_TC_ROOT}/staging")
endif()

if(CMAKE_SYSROOT)
    set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
else()
    execute_process(COMMAND "${CMAKE_C_COMPILER}" -print-sysroot
                    OUTPUT_VARIABLE _BUNDLED_SYSROOT OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(CMAKE_FIND_ROOT_PATH "${_BUNDLED_SYSROOT}")
    message(STATUS "doorguard sysroot 未找到,暂用编译器自带 libc(仅基础库)")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
