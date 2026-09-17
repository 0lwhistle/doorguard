/*
 * dg_test.h — door-guard 最小测试框架(断言宏 + 计数)
 *
 * 为什么不用现成框架:门禁测试是纯 C、宿主 gcc 直跑,10 行宏够用且零依赖;
 * 每个测试一个 main,ctest 粒度=可执行文件,失败定位直接到文件:行号。
 */
#ifndef DG_TEST_H
#define DG_TEST_H

#include <stdio.h>

static int dg_pass = 0, dg_fail = 0;

#define DG_CHECK(cond) do { \
    if (cond) { dg_pass++; } \
    else { dg_fail++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

/* main 尾部调用:打印汇总并返回进程退出码(0=全过) */
#define DG_TEST_EXIT() do { \
    printf("%s: %d passed, %d failed\n", __FILE__, dg_pass, dg_fail); \
    return dg_fail ? 1 : 0; \
} while (0)

#endif /* DG_TEST_H */
