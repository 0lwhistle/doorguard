/*
 * test_registry.c — services 注册表测试(M2③;纯逻辑,宿主可跑)
 *
 * 覆盖:注册语义(重名/未知查询);依赖分批初始化(顺序/缺失/循环);
 * 必需 vs 选修(init_all 返回值语义);跨表依赖解析器;看门狗原语
 * (restart 计数/disabled 幂等/重启被拒);巡检判定(健康/心跳超龄/ERROR)。
 */
#include "dg_test.h"
#include "registry.h"

#include <stdio.h>
#include <string.h>

static int ok_start(void)  { return 0; }
static int fail_start(void) { return -1; }

/* 翻转型:前 N 次失败,之后成功(restart 恢复路径) */
static int flip_state = 0;
static int flip_start(void)
{
    return flip_state++ < 1 ? -1 : 0;        /* 第 1 次失败,第 2 次成功 */
}

/* 心跳钩子:固定返回"很旧"的时间戳 */
static int64_t hb_stale(void) { return 1000; }   /* unix ms 纪元附近 = 极旧 */

static void reset(void)
{
    registry_destroy();
    registry_init();
    registry_set_dep_resolver(NULL);
    flip_state = 0;
}

/* ---- R1 注册语义 ---- */
static void test_register_semantics(void)
{
    printf("[R1] register semantics\n");
    reset();
    DG_CHECK(registry_register(NULL, ok_start, true, NULL, 0, NULL, NULL)
             == REG_ERR_INVALID_PARAM);
    DG_CHECK(registry_register("a", NULL, true, NULL, 0, NULL, NULL)
             == REG_ERR_INVALID_PARAM);
    DG_CHECK(registry_register("a", ok_start, true, NULL, 0, NULL, NULL) == REG_OK);
    DG_CHECK(registry_register("a", ok_start, true, NULL, 0, NULL, NULL)
             == REG_ERR_ALREADY_REGISTERED);
    DG_CHECK(registry_state("nope") == REG_STATE_UNKNOWN);
    DG_CHECK(registry_state("a") == REG_STATE_REGISTERED);
    DG_CHECK(registry_count() == 1);
    DG_CHECK(strcmp(registry_name_at(0), "a") == 0);
    DG_CHECK(registry_name_at(1) == NULL);
}

/* ---- R2 依赖分批:顺序/缺失/循环 ---- */
static void test_dep_batches(void)
{
    printf("[R2] dependency batches (order / missing / cycle)\n");
    reset();
    const char *const B_DEP[] = { "a" };
    DG_CHECK(registry_register("b", ok_start, false, B_DEP, 1, NULL, NULL) == REG_OK);
    DG_CHECK(registry_register("a", ok_start, false, NULL, 0, NULL, NULL) == REG_OK);
    DG_CHECK(registry_init_all(true) == REG_OK);
    DG_CHECK(registry_is_ready("a") && registry_is_ready("b"));   /* a 先就绪 */

    reset();
    const char *const E_DEP[] = { "ghost" };
    DG_CHECK(registry_register("e", ok_start, false, E_DEP, 1, NULL, NULL) == REG_OK);
    DG_CHECK(registry_init_all(true) == REG_OK);        /* 选修缺依赖:不阻塞 */
    DG_CHECK(registry_state("e") == REG_STATE_ERROR);

    reset();
    const char *const C_DEP[] = { "d" };
    const char *const D_DEP[] = { "c" };
    DG_CHECK(registry_register("c", ok_start, false, C_DEP, 1, NULL, NULL) == REG_OK);
    DG_CHECK(registry_register("d", ok_start, false, D_DEP, 1, NULL, NULL) == REG_OK);
    DG_CHECK(registry_init_all(true) == REG_OK);        /* 循环:置 ERROR 不死循环 */
    DG_CHECK(registry_state("c") == REG_STATE_ERROR);
    DG_CHECK(registry_state("d") == REG_STATE_ERROR);
}

/* ---- R3 跨表依赖解析器 ---- */
static int resolver_ready(const char *name)     /* "holder_mod" 总是就绪 */
{
    (void)name;
    return 1;
}
static void test_dep_resolver(void)
{
    printf("[R3] injected dep resolver\n");
    reset();
    const char *const F_DEP[] = { "holder_mod" };
    DG_CHECK(registry_register("f", ok_start, false, F_DEP, 1, NULL, NULL) == REG_OK);
    DG_CHECK(registry_init_all(true) == REG_OK);
    DG_CHECK(registry_state("f") == REG_STATE_ERROR);   /* 无解析器:表外依赖不可满足 */

    reset();
    registry_set_dep_resolver(resolver_ready);
    DG_CHECK(registry_register("f", ok_start, false, F_DEP, 1, NULL, NULL) == REG_OK);
    DG_CHECK(registry_init_all(true) == REG_OK);
    DG_CHECK(registry_is_ready("f"));                   /* 解析器放行 */
}

/* ---- R4 必需 vs 选修 ---- */
static void test_required_policy(void)
{
    printf("[R4] required vs optional\n");
    reset();
    DG_CHECK(registry_register("opt", fail_start, false, NULL, 0, NULL, NULL) == REG_OK);
    DG_CHECK(registry_register("req", fail_start, true, NULL, 0, NULL, NULL) == REG_OK);
    DG_CHECK(registry_init_all(false) == REG_OK);       /* 不停:只记录 */
    DG_CHECK(registry_state("opt") == REG_STATE_ERROR);
    DG_CHECK(registry_state("req") == REG_STATE_ERROR);
    DG_CHECK(registry_is_required("req") && !registry_is_required("opt"));

    reset();
    DG_CHECK(registry_register("req", fail_start, true, NULL, 0, NULL, NULL) == REG_OK);
    DG_CHECK(registry_init_all(true) == REG_ERR_DEPENDENCY);  /* 必需失败即报 */
}

/* ---- R5 看门狗原语:restart / disabled / 巡检判定 ---- */
static void test_watchdog_primitives(void)
{
    printf("[R5] restart / disabled / watchdog poll\n");
    reset();
    flip_state = 0;
    DG_CHECK(registry_register("g", flip_start, false, NULL, 0, hb_stale, NULL)
             == REG_OK);
    DG_CHECK(registry_init_all(true) == REG_OK);        /* 选修:失败不阻塞 */
    DG_CHECK(registry_state("g") == REG_STATE_ERROR);
    DG_CHECK(registry_restart_count("g") == 0);
    DG_CHECK(registry_restart("g") == REG_OK);          /* 第 2 次成功 */
    DG_CHECK(registry_is_ready("g") && registry_restart_count("g") == 1);

    /* 巡检:READY 但心跳极旧 → STALE */
    DG_CHECK(registry_watchdog_poll(2000000, 60000) == REG_WATCH_STALE);
    /* 心跳钩子返回 <=0:视为无数据,不判失联 */
    reset();
    DG_CHECK(registry_register("h", ok_start, false, NULL, 0, NULL, NULL) == REG_OK);
    DG_CHECK(registry_init_all(true) == REG_OK);
    DG_CHECK(registry_watchdog_poll(2000000, 60000) == REG_WATCH_OK);
    /* ERROR 态 → 巡检报 ERROR */
    reset();
    DG_CHECK(registry_register("k", fail_start, false, NULL, 0, NULL, NULL) == REG_OK);
    DG_CHECK(registry_init_all(true) == REG_OK);
    DG_CHECK(registry_watchdog_poll(2000000, 60000) == REG_WATCH_ERROR);

    /* disabled:重启被拒、幂等 */
    DG_CHECK(registry_mark_disabled("k") == REG_OK);
    DG_CHECK(registry_mark_disabled("k") == REG_OK);    /* 幂等 */
    DG_CHECK(registry_state("k") == REG_STATE_DISABLED);
    DG_CHECK(registry_restart("k") == REG_ERR_STATE);
    DG_CHECK(registry_mark_disabled("nope") == REG_ERR_NOT_FOUND);
}

int main(void)
{
    test_register_semantics();
    test_dep_batches();
    test_dep_resolver();
    test_required_policy();
    test_watchdog_primitives();

    registry_destroy();
    DG_TEST_EXIT();
}
