# tests/ — ctest 用例(宿主 gcc,dg-test 驱动)
#
# 每个测试一个可执行文件、一个 main:失败定位直接到 文件:行号;
# TIMEOUT 防死锁挂死 CI/闲时任务。demo_* 是模块使用示例,作为冒烟用例一并跑。
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
dg_add_test(test_proto dg_proto)

# 模块 demo 冒烟(使用示例须始终可运行)
macro(dg_add_demo name src)
    add_executable(${name} ${src})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/proto)
    target_link_libraries(${name} PRIVATE ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
endmacro()

dg_add_demo(demo_event_bus ${CMAKE_SOURCE_DIR}/proto/event_bus/demo_event_bus.c dg_event_bus)
dg_add_demo(demo_tasker ${CMAKE_SOURCE_DIR}/proto/tasker/demo_tasker.c dg_tasker)
dg_add_demo(demo_holder ${CMAKE_SOURCE_DIR}/proto/holder/demo_holder.c dg_holder)
