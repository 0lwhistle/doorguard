# 模块开发指南(door-guard)

> 面向"我要开发/修改/新增一个模块"。全局架构见 `.agents/skills/door-guard-dev/references/architecture.md`;
> 本文只讲**落到哪个目录、加哪些文件、按什么约定写**。业务规格各自见 references/spec-*.md。

## 0. 先决定它属于哪一层(这一步错了后面全白干)

```
ui/          页面与弹窗(LVGL)。只发事件/收事件,不做业务决策,不碰 DB
services/    业务编排(认证/录入/门控/网络服务族)。注册进 registry,线程化
modules/     设备与基础设施(camera/display/sqlite/net/touch/as608/mfrc522)。注册进 holder
drv/         总线级薄封装(uart/gpio/npu/i2c/spi)。PC 模拟器 = 同接口 sim 后端
components/  机制(不装业务):tasker/event_bus/holder/registry/logger
proto/       唯一横切契约层:events·types·valid·err(被所有层引用)
third_party/ 第三方(vendored,不改内容,告警豁免)
```

**硬边界**:跨层只能经 `proto/` 的事件总线;服务间不直接调用(例外=只读直调登记制,
新登记必须先写进 architecture proposal §1)。UI 不碰 SQL,服务不碰 LVGL。

## 1. 要加哪些文件(按层的清单)

| 场景 | 文件 | 位置 |
|---|---|---|
| 新模块/驱动 | `<name>.h` + `<name>.c`(板端实现) | `modules/<name>/` 或 `drv/<name>/` |
| 同上,需 PC 模拟器 | `<name>_sim.c`(同接口 sim 实现) | 同目录 |
| 新服务 | `<name>.h` + `<name>.c` | `services/<name>/` |
| 新页面(UI) | `page_<name>.c` + presenter | `ui/pages/` + `ui/presenters/` |
| 新控件 | `dg_<name>.c/.h` | `ui/widgets/` |
| 事件/结构/错误码 | 改 `proto/events.h` / `proto/types.h` / `proto/err.h` | `proto/` |
| 测试 | `test_<name>.c` | `tests/` |
| 模块文档 | `README.md`(职责/坑/环境开关/使用示例) | 模块目录内 |
| 构建 | 改 `CMakeLists.txt`(加 `add_library` + 在 `tests/CMakeLists.txt` 加 `dg_add_test`) | 仓库根 |
| 装配 | 改 `app/main.c` 的 `table[]`(modules)或 `svc[]`(services) | `app/` |
| 配置项 | `configs/default.json`(出厂模板);代码零魔数 | `configs/` |

模拟器双端不是可选项:凡涉及硬件的模块都要有 `_sim.c`,否则宿主 31 个测试跑不起来。

## 2. 代码框架(照抄这个骨架)

**模块头(`modules/foo/foo.h`)** —— 只暴露必要 API,错误码统一 `int`(0 成功/负错误码):

```c
#ifndef DG_FOO_H
#define DG_FOO_H
#include <stdint.h>
#include <stdbool.h>
#include "err.h"

/* 初始化。失败返回负错误码(见 proto/err.h);可被 holder 调用 */
int foo_init(const char *res_path);

/* 轮询泵(主循环调用;若有自己的线程则不必) */
void foo_poll(void);

/* 只读查询:调用方只读,模块持有内存,下次 poll 前有效 */
const foo_frame_t *foo_latest(void);

#endif /* DG_FOO_H */
```

**模块实现(`modules/foo/foo.c`)** —— 日志带模块 tag,错误显式返回不吞:

```c
#include "foo.h"
#include "dg_log.h"          /* components/logger */
#include <string.h>
#include <errno.h>

#define FOO_TAG "[FOO]"

static struct { bool ready; } s_foo;

int foo_init(const char *res_path)
{
    if (!res_path || !*res_path)      /* 边界:空输入显式失败,不静默 */
        return DG_ERR_PARAM;
    /* … */
    DG_LOGI(FOO_TAG, "就绪:%s", res_path);
    s_foo.ready = true;
    return DG_OK;
}
```

**测试(`tests/test_foo.c`)** —— 纯 C、宿主 gcc、一个文件一个 main:

```c
#include "dg_test.h"
#include "foo.h"

int main(void)
{
    DG_CHECK(foo_init(NULL) == DG_ERR_PARAM);     /* 边界必测 */
    DG_CHECK(foo_init("/dev/foo") == DG_OK);
    foo_poll();
    DG_CHECK(foo_latest() != NULL);
    DG_TEST_EXIT();
}
```

**构建注册(仓库根 `CMakeLists.txt`)**:

```cmake
add_library(dg_foo STATIC modules/foo/foo.c)          # sim 另加 *_sim.c(按端条件编译)
target_include_directories(dg_foo PUBLIC ${CMAKE_SOURCE_DIR}/modules/foo)
target_link_libraries(dg_foo PUBLIC dg_log)           # 依赖谁就链谁
```

然后在 `tests/CMakeLists.txt` 加一行 `dg_add_test(test_foo dg_foo)`,
在 `app/main.c` 的 `table[]` 里登记(模块)或 `svc[]`(服务)——含 `required`
与 `deps`,holder/registry 据此按依赖顺序初始化。

## 3. 约定俗成的 API(别自造)

| 用途 | API | 出处 |
|---|---|---|
| 日志 | `DG_LOGI/W/E(TAG, fmt, …)`,TAG=`"[模块名大写]"` | `components/logger/dg_log.h` |
| 错误码 | `DG_OK=0` / 负码;分段:通用 -1~-19、用户 -20~-29、验证 -30~-39、网络 -40~-49 | `proto/err.h` |
| 发事件 | `event_bus_publish(EV_X, &payload, sizeof payload)` | `components/event_bus/event_bus.h` |
| 订阅 | `event_bus_subscribe(EV_X, cb, ud)` / `event_bus_unsubscribe(h)` | 同上 |
| 定义事件 | `proto/events.h` 里 `EV_DEF(DG_MODULE_ID_X, 0xNNNN)`,负载 ≤256B 且加 `_Static_assert` | `proto/events.h` |
| 模块注册 | `holder_register_module_ex(name, fn, required, deps, n_deps, hb)` | `components/holder/holder.h` |
| 服务注册 | `registry_register(name, fn, required, deps, n, hb, ud)` | `components/registry/registry.h` |
| 配置读取 | `cfg_get()->字段`(业务参数进 JSON,代码零魔数) | `services/config/cfg.h` |
| 线程化 | 短任务用 `tasker`;长线程自建 pthread(阻塞点绝不放主循环) | `components/tasker/` |

**命名**:`模块_动作()` 函数、`模块_类型_t` 类型、文件名带模块前缀;
注释解释"为什么"(约束/坑),不复述代码。

## 4. 硬性规范(踩过坑,别商量)

- **边界优先**:动手前先列边界(空输入/超时/重复/权限不足/并发/掉电恢复),
  每个显式处理并返回错误码,**禁止静默吞掉**。
- **UI**:所有 label 一律 `_("原文")` 包裹,译文进 `ui/lang/<lang>.json`;
  按钮=图标+文字;色值走 `ui/theme.h` token;新页面/容器一律
  `lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE)`(否则吞点击,实测 v9 必踩)。
- **完成的定义** = 代码 + 注释 + 测试(宿主可跑)+ 模块 README(含使用示例)+
  README 里登记环境开关。缺一不算完。
- **只提交经 git 流转**:`git push origin master`;禁止跨机拷贝目录。
- **文件边界**:改 UI 别动 services;`configs/cur_config.json` 是运行时状态,不提交。
- 推板前 `dg-build` **零告警** + `dg-test` **全绿**;部署后 md5 三方核对
  (宿主 build = 板上文件 = `/proc/pid/exe`)。

## 5. 工作流

```bash
source env/env.sh            # 仓库任意位置;dg-* 进 PATH
dg-build                     # 交叉编译(-c 全新配置)
dg-test                      # 宿主 ctest
dg-deploy -r 192.168.x.x     # 推板并运行(先 stop S60)
```

改完一处 → `dg-build` 零告警 → `dg-test` → 板上验(硬件相关必须板上实测,
模拟器只证明逻辑)→ `docs/DEVLOG.md` 顶部追加(≤30 行)→ commit+push。

## 6. 参考实现(照着一个像的抄)

| 想抄什么 | 看谁 |
|---|---|
| 双端模块(sim+板) | `modules/camera/`(README 列了 3 个致命坑,值得通读) |
| 服务 + 事件 + 门禁决策 | `services/access/` |
| 页面 + presenter | `ui/pages/page_home.c` + `ui/presenters/` |
| 硬件驱动薄封装 | `drv/npu/`、`drv/uart/` |
| 板端走查/取证 | `tools/board-walk/README.md`(注入库/导图/CPU 采样) |
