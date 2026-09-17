# holder — 全局模块注册表

## 出处

ESP32 模板 `ovs` 的 `components/modules/holder/`(holder.h/holder.c)。
模板路径:`/home/olwhistle/dockerNow/esp32/programs/ovs`。

## door-guard 用途

HAL 与服务的启动顺序与健康状态注册表:`app/main.c` 装配时按"注册(可带依赖)
→ `holder_init_all` → 必需模块失败即退出(systemd 拉起重试)"模式使用;
运行期经 `holder_is_module_ready` 做健康检查(如摄像头断流降级)。

## port 改动点

| 项 | 模板 | 本移植 | 原因 |
|---|---|---|---|
| 互斥锁 | FreeRTOS `xSemaphoreCreateMutex`/Take/Give | pthread 互斥量 + `pthread_mutex_timedlock`(1000ms 超时语义等价) | Linux 平台 |
| 计时 | `esp_timer_get_time()` | `CLOCK_MONOTONIC` 微秒 | 初始化耗时统计 |
| mem/logger | 模板 mem.h / logger.h | malloc/free / `proto/dg_log.h` | 去模板依赖 |
| 销毁判空 | `if (mutex) vSemaphoreDelete(mutex)` | 直接 `pthread_mutex_destroy`(依赖 initialized 标志防重复) | pthread 互斥量为内联类型非指针 |

核心逻辑**逐行保留**:按依赖分批初始化(轮次"解锁"下一批)、缺失依赖/
循环依赖在 `holder_init_all` 收尾统一置 ERROR(附"Dependency not satisfied
or cycle detected")、ERROR 态不影响其他模块、必需模块可选是否阻止启动。

## 使用示例

```c
#include "holder.h"

static int storage_init(void) { /* 打开 SQLite */ return 0; }
static const char *const deps[] = { "storage" };

holder_init();
holder_register_module("storage", storage_init, true, NULL);
holder_register_module_ex("access", access_init, true, deps, 1, NULL);
if (holder_init_all(true /*必需模块失败即停*/) != HOLDER_OK)
    exit(1);                       /* 交给 systemd 拉起重试 */
holder_is_module_ready("storage"); /* 运行期健康查询 */
holder_destroy();
```

完整可运行示例:`demo_holder.c`(ctest 冒烟)。

## 注意事项

- 依赖必须先注册且最终 READY,否则本模块被置 ERROR;循环依赖双方都 ERROR
- 单个模块 init 失败只置 ERROR + 计数,不拖垮注册表(隔离性,tests/test_holder.c 验证)
- `holder_init_module` 单独初始化时若依赖未就绪返回 `HOLDER_ERR_DEPENDENCY`,
  请走 `holder_init_all`
