/*
 * test_holder.c — holder 移植测试(Phase 1 新增)
 *
 * 覆盖任务要求:依赖循环检测、重复注册、ERROR 态隔离(单模块失败不拖垮
 * 注册表);另覆盖缺失依赖、必需模块失败停机、就绪查询。
 */
#include "holder.h"
#include "dg_test.h"

#include <string.h>

static int init_ok(void)   { return 0; }
static int init_bad(void)  { return -1; }        /* 模拟模块自身故障 */

/* 各用例独立注册表:destroy 保证互不串扰 */
static void reset_holder(void)
{
    holder_destroy();
    DG_CHECK(holder_init() == HOLDER_OK);
}

/* ---------- H1 重复注册 ---------- */
static void test_duplicate_register(void)
{
    printf("[H1] duplicate registration rejected\n");
    reset_holder();

    DG_CHECK(holder_register_module("storage", init_ok, true, NULL) == HOLDER_OK);
    DG_CHECK(holder_register_module("storage", init_ok, true, NULL)
             == HOLDER_ERR_ALREADY_REGISTERED);
    DG_CHECK(holder_get_module_count() == 1);

    /* 非法参数 */
    DG_CHECK(holder_register_module(NULL, init_ok, true, NULL)
             == HOLDER_ERR_INVALID_PARAM);
    DG_CHECK(holder_register_module("bad", NULL, true, NULL)
             == HOLDER_ERR_INVALID_PARAM);
}

/* ---------- H2 依赖顺序初始化 ---------- */
static const char *const deps_on_storage[] = { "storage" };
static const char *const deps_on_access[]  = { "access" };

static void test_dependency_order(void)
{
    printf("[H2] dependency-ordered init (storage -> access -> ui)\n");
    reset_holder();

    /* 故意按 ui → access → storage 的乱序注册 */
    DG_CHECK(holder_register_module_ex("ui", init_ok, true,
                                       deps_on_access, 1, NULL) == HOLDER_OK);
    DG_CHECK(holder_register_module_ex("access", init_ok, true,
                                       deps_on_storage, 1, NULL) == HOLDER_OK);
    DG_CHECK(holder_register_module("storage", init_ok, true, NULL) == HOLDER_OK);

    DG_CHECK(holder_init_all(false) == HOLDER_OK);
    DG_CHECK(holder_is_module_ready("storage"));
    DG_CHECK(holder_is_module_ready("access"));
    DG_CHECK(holder_is_module_ready("ui"));

    /* 依赖未就绪时单独初始化被拒绝(防绕过顺序) */
    reset_holder();
    DG_CHECK(holder_register_module_ex("ui", init_ok, true,
                                       deps_on_access, 1, NULL) == HOLDER_OK);
    DG_CHECK(holder_init_module("ui") == HOLDER_ERR_DEPENDENCY);
    DG_CHECK(holder_get_module_state("ui") == HOLDER_MODULE_STATE_REGISTERED);
}

/* ---------- H3 循环依赖:双方 ERROR,第三方不受影响 ---------- */
static void test_dependency_cycle(void)
{
    printf("[H3] dependency cycle -> both ERROR, isolation for others\n");
    reset_holder();

    /* 干净的两模块环:cyc_a -> cyc_b -> cyc_a */
    static const char *const dep_b[] = { "cyc_b" };
    static const char *const dep_a[] = { "cyc_a" };
    DG_CHECK(holder_register_module_ex("cyc_a", init_ok, true, dep_b, 1, NULL) == HOLDER_OK);
    DG_CHECK(holder_register_module_ex("cyc_b", init_ok, true, dep_a, 1, NULL) == HOLDER_OK);
    DG_CHECK(holder_register_module("standalone", init_ok, true, NULL) == HOLDER_OK);

    DG_CHECK(holder_init_all(false) == HOLDER_OK);   /* 非停机模式:环不算全局失败 */
    DG_CHECK(holder_get_module_state("cyc_a") == HOLDER_MODULE_STATE_ERROR);
    DG_CHECK(holder_get_module_state("cyc_b") == HOLDER_MODULE_STATE_ERROR);
    DG_CHECK(holder_is_module_ready("standalone"));  /* 隔离:第三方照常就绪 */
}

/* ---------- H4 缺失依赖 + ERROR 态隔离 ---------- */
static void test_missing_dep_and_isolation(void)
{
    printf("[H4] missing dependency / init failure isolation\n");
    reset_holder();

    /* ui 依赖 "access",但 access 从未注册 */
    DG_CHECK(holder_register_module_ex("ui", init_ok, true,
                                       deps_on_access, 1, NULL) == HOLDER_OK);
    DG_CHECK(holder_register_module("storage", init_ok, true, NULL) == HOLDER_OK);
    DG_CHECK(holder_register_module("camera", init_bad, false, NULL) == HOLDER_OK);

    DG_CHECK(holder_init_all(false) == HOLDER_OK);
    /* ui(缺依赖)与 camera(init 失败)都 ERROR,storage 照常就绪 */
    DG_CHECK(holder_get_module_state("ui") == HOLDER_MODULE_STATE_ERROR);
    DG_CHECK(holder_get_module_state("camera") == HOLDER_MODULE_STATE_ERROR);
    DG_CHECK(holder_is_module_ready("storage"));
    holder_module_info_t info;
    DG_CHECK(holder_get_module_info("camera", &info) == HOLDER_OK);
    DG_CHECK(info.error_count == 1);
    DG_CHECK(info.last_error != NULL && strlen(info.last_error) > 0);
}

/* ---------- H5 必需模块失败 + stop_on_required_error ---------- */
static void test_required_failure(void)
{
    printf("[H5] required module failure honours stop_on_required_error\n");
    reset_holder();

    DG_CHECK(holder_register_module("core", init_bad, true, NULL) == HOLDER_OK);
    DG_CHECK(holder_register_module("aux", init_ok, true, NULL) == HOLDER_OK);

    DG_CHECK(holder_init_all(true) == HOLDER_ERR_INIT_FAILED);
    DG_CHECK(holder_get_module_state("core") == HOLDER_MODULE_STATE_ERROR);

    /* 不停机模式:返回 OK,失败模块只置 ERROR */
    reset_holder();
    DG_CHECK(holder_register_module("core", init_bad, true, NULL) == HOLDER_OK);
    DG_CHECK(holder_register_module("aux", init_ok, true, NULL) == HOLDER_OK);
    DG_CHECK(holder_init_all(false) == HOLDER_OK);
    DG_CHECK(holder_get_module_state("core") == HOLDER_MODULE_STATE_ERROR);
    DG_CHECK(holder_is_module_ready("aux"));
}

/* ---------- H6 生命周期与查询边界 ---------- */
static void test_lifecycle(void)
{
    printf("[H6] lifecycle / query edges\n");
    holder_destroy();                           /* 未初始化时 destroy 应为无害空操作 */
    DG_CHECK(!holder_is_initialized());
    DG_CHECK(holder_get_module_state("anything") == HOLDER_MODULE_STATE_UNKNOWN);
    DG_CHECK(holder_get_module_count() == 0);

    DG_CHECK(holder_init() == HOLDER_OK);
    DG_CHECK(holder_init() == HOLDER_OK);       /* 幂等(模板语义) */
    DG_CHECK(holder_is_initialized());

    DG_CHECK(holder_register_module("m", init_ok, true, NULL) == HOLDER_OK);
    DG_CHECK(holder_get_module_count() == 1);
    DG_CHECK(strcmp(holder_get_module_name(0), "m") == 0);
    DG_CHECK(holder_get_module_name(1) == NULL); /* 越界 */
    DG_CHECK(holder_get_module_info("nope", NULL) == HOLDER_ERR_INVALID_PARAM);
    holder_module_info_t qinfo;
    DG_CHECK(holder_get_module_info("nope", &qinfo) == HOLDER_ERR_NOT_FOUND);
    DG_CHECK(holder_unregister_module("m") == HOLDER_OK);
    DG_CHECK(holder_unregister_module("m") == HOLDER_ERR_NOT_FOUND);
}

int main(void)
{
    test_duplicate_register();
    test_dependency_order();
    test_dependency_cycle();
    test_missing_dep_and_isolation();
    test_required_failure();
    test_lifecycle();
    holder_destroy();

    DG_TEST_EXIT();
}
