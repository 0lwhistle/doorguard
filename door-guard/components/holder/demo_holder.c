/*
 * demo_holder.c — holder 使用示例(可执行,ctest 冒烟跑通)
 *
 * 演示 door-guard 装配的标准姿势(app/main.c 将按此模式装配 HAL/服务):
 * 注册带依赖的模块 → init_all 按依赖分批初始化 → 健康查询。
 */
#include "holder.h"

#include <stdio.h>
#include <string.h>

static int init_ok(void)   { return 0; }
static int init_bad(void)  { return -1; }        /* 模拟可选模块初始化失败 */

static const char *const deps_on_db[] = { "storage" };

int main(void)
{
    if (holder_init() != HOLDER_OK) {
        fprintf(stderr, "demo: holder_init 失败\n");
        return 1;
    }

    /* storage 是必需模块;access 依赖 storage;camera 是可选模块且会失败 */
    holder_register_module("storage", init_ok, true, NULL);
    holder_register_module_ex("access", init_ok, true,
                              deps_on_db, 1, NULL);
    holder_register_module("camera", init_bad, false, NULL);

    /* 非必需模块失败不阻止整体启动 */
    if (holder_init_all(false) != HOLDER_OK) {
        fprintf(stderr, "demo: holder_init_all 失败\n");
        return 1;
    }

    if (!holder_is_module_ready("storage") || !holder_is_module_ready("access") ||
        holder_is_module_ready("camera")) {
        fprintf(stderr, "demo: 模块状态与预期不符\n");
        return 1;
    }

    /* 重复注册被拒绝 */
    if (holder_register_module("storage", init_ok, true, NULL)
        != HOLDER_ERR_ALREADY_REGISTERED) {
        fprintf(stderr, "demo: 重复注册未被拒绝\n");
        return 1;
    }

    holder_destroy();
    printf("demo_holder: OK (storage/access READY, camera ERROR 不拖垮注册表)\n");
    return 0;
}
