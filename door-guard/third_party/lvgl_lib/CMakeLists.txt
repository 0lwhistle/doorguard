# LVGL v9.5.0 ESP-IDF 包装组件
# 上游源码: thirdparty/lvgl-9.5.0（原样 vendor，勿改动上游文件）
# lv_conf.h: src/lvgl/lv_conf.h（双平台共用，经 LV_CONF_INCLUDE_SIMPLE 注入）
# PC 模拟器不走本组件，见 sim/CMakeLists.txt（上游 os_desktop.cmake 路线）

set(LVGL_ROOT ${CMAKE_CURRENT_LIST_DIR}/../lvgl-9.5.0)

file(GLOB_RECURSE LVGL_SRCS ${LVGL_ROOT}/src/*.c)
# 不做目录排除（与上游 esp.cmake 行为一致）：
#   - 未启用的可选库/驱动由 LV_USE_* 宏守卫，编译为空翻译单元，无害
#   - thorvg 等 C++ 源为 .cpp，*.c 通配自动排除
#   - src/libs/bin_decoder 等是 lv_init 必需解码器，不可排除

idf_component_register(
    SRCS ${LVGL_SRCS}
    INCLUDE_DIRS ${LVGL_ROOT} ${LVGL_ROOT}/src
)

# lv_conf.h 位于 src/lvgl（组件目录外，全局唯一），PUBLIC 传播给 UI 层
target_include_directories(${COMPONENT_LIB} PUBLIC ${CMAKE_SOURCE_DIR}/src/lvgl)
target_compile_definitions(${COMPONENT_LIB} PUBLIC LV_CONF_INCLUDE_SIMPLE)
