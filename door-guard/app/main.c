/*
 * door-guard main — 模块装配与生命周期
 *
 * 装配方式:holder 注册表(proto/holder/README.md;B7 §0.2 注册表)。
 * 每个模块包一层 `int(void)` init_fn,依赖用 holder_register_module_ex 声明,
 * holder_init_all(true) 按依赖分批初始化:
 *   - 必需模块失败 → 直接退出(S60 监督循环 3s 拉起;这是原有"故障即退出"语义)
 *   - 选修模块失败 → 记 ERROR 继续(相机/视觉/UI 挂了,门禁与 web 仍可用)
 * 运行期健康:holder_is_module_ready("vision_backend") 等。
 *
 * 启动顺序(依赖链,注册顺序即序):
 *   event_bus → tasker → storage → config(cfg 加载)
 *     → camera → capture → vision_service → vision_backend
 *     → access → enroll → liveness → web → mdns → ui(display 在 ui 内初始化)
 *
 * 模块间只经 proto/ 定义的队列与事件总线通信,不直接互调。
 * #ifdef DG_SIM 仅出现在本装配与 HAL sim 后端(纪律允许范围)。
 */
#include "hal/camera/camera.h"
#include "access_service.h"
#include "capture_service.h"
#include "cfg.h"
#include "dg_log.h"
#include "enroll_service.h"
#include "event_bus.h"
#include "holder.h"
#include "liveness_service.h"
#include "mdns/mdns_responder.h"
#include "ntp/ntp_service.h"
#include "web/web_server.h"
#include "storage.h"
#include "tasker.h"
#include "ui.h"
#include "vision_service.h"


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* 板上路径(spec-database);/data 分区待 B10 固件统一(DEVLOG 待确认)。
 * 模拟器用仓库内 sim/data,避免开发机写系统目录 */
#ifdef DG_SIM
#define DG_DB_PATH  "sim/data/door-guard.db"
#define DG_KEY_PATH "sim/data/dg.key"
#else
#define DG_DB_PATH  "/var/lib/door-guard/door-guard.db"
#define DG_KEY_PATH "/var/lib/door-guard/dg.key"
#endif

/* 装配参数(依赖 init_fn 无参数,故经静态变量传给包装函数) */
static const char *s_json_path;
static dg_ui_args_t s_ui_args;
static const char *s_camera_dir;

/* ---- 必需模块 ---- */

static int mod_event_bus(void)
{
    return event_bus_init() == EVENT_BUS_OK ? DG_OK : DG_ERR_INTERNAL;
}

static int mod_tasker(void)
{
#ifdef DG_SIM
    system("mkdir -p sim/data lang configs");   /* 模拟器工作目录(首次运行创建) */
#endif
    return tasker_init() == TASK_OK ? DG_OK : DG_ERR_INTERNAL;
}

static int mod_storage(void)
{
    return storage_init(DG_DB_PATH, DG_KEY_PATH);
}

/* cfg_load 的 json 路径:板上 /etc,模拟器取仓库 configs/(开发便利) */
static int mod_config(void)
{
    return cfg_load(s_json_path);
}

static int mod_vision_service(void)
{
    return vision_service_start();
}

static int mod_access(void)
{
    return access_service_start();
}

/* ---- 选修模块(失败记 ERROR 继续;缺相机/视觉时门禁与 web 仍可用) ---- */

static int mod_camera(void)
{
    return camera_init(s_camera_dir, NULL, NULL);
}

static int mod_capture(void)
{
    return capture_service_start();
}

/* 视觉后端:init_fn 返回非 0 → holder 置 ERROR(缺模型时降级为无检测,
 * 不阻塞其余业务;失败原因见 [VISION] 日志行) */
static int mod_vision_backend(void)
{
#ifdef DG_SIM
    /* 模拟器默认开 mock(DG_SIM_VISION=0 关);板上恒 false */
    bool enable_mock = (getenv("DG_SIM_VISION") == NULL ||
                        strcmp(getenv("DG_SIM_VISION"), "0") != 0);
    return vision_backend_start(enable_mock);
#else
    return vision_backend_start(false);
#endif
}

static int mod_enroll(void)
{
    return enroll_service_start();
}

static int mod_liveness(void)
{
    return liveness_service_start();
}

static int mod_web(void)
{
    return web_server_start();
}

static int mod_mdns(void)
{
    return mdns_start();
}

/* UI 失败仅告警(板上 web 上位机路径照常) */
static int mod_ui(void)
{
    return ui_init(&s_ui_args);
}

/* 注册表:顺序 = 依赖序(§0.2);依赖由 holder 校验并按批初始化 */
static const char *const DEP_EVENT_BUS[]  = { "event_bus" };
static const char *const DEP_TASKER[]     = { "tasker" };
static const char *const DEP_STORAGE[]    = { "storage" };
static const char *const DEP_CONFIG[]     = { "config" };
static const char *const DEP_CAMERA[]     = { "config", "camera" };
static const char *const DEP_VIS_SVC[]    = { "event_bus" };
static const char *const DEP_VIS_BE[]     = { "vision_service", "camera" };
static const char *const DEP_SVC[]        = { "tasker" };
static const char *const DEP_NET[]        = { "config" };

static int register_modules(void)
{
    struct {
        const char *name;
        holder_module_init_fn fn;
        bool required;
        const char *const *deps;
        int n_deps;
    } table[] = {
        { "event_bus",      mod_event_bus,      true,  NULL,          0 },
        { "tasker",         mod_tasker,         true,  DEP_EVENT_BUS, 1 },
        { "storage",        mod_storage,        true,  DEP_TASKER,    1 },
        { "config",         mod_config,         true,  DEP_STORAGE,   1 },
        { "camera",         mod_camera,         false, DEP_CONFIG,    1 },
        { "capture",        mod_capture,        false, DEP_CAMERA,    2 },
        { "vision_service", mod_vision_service, true,  DEP_VIS_SVC,   1 },
        { "vision_backend", mod_vision_backend, false, DEP_VIS_BE,    2 },
        { "access",         mod_access,         true,  DEP_SVC,       1 },
        { "enroll",         mod_enroll,         false, DEP_SVC,       1 },
        { "liveness",       mod_liveness,       false, DEP_SVC,       1 },
        { "web",            mod_web,            false, DEP_NET,       1 },
        { "mdns",           mod_mdns,           false, DEP_NET,       1 },
        /* ui 依赖 display:display 由 ui_init 内部初始化(无独立模块),
         * 故此处只声明 config(语言/主题取 cfg) */
        { "ui",             mod_ui,             false, DEP_CONFIG,    1 },
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        holder_err_t rc = holder_register_module_ex(table[i].name, table[i].fn,
                                                   table[i].required,
                                                   table[i].deps, table[i].n_deps,
                                                   NULL);
        if (rc != HOLDER_OK) {
            fprintf(stderr, "holder 注册 %s 失败(%d)\n", table[i].name, rc);
            return DG_ERR_INTERNAL;
        }
    }
    return DG_OK;
}

int main(int argc, char *argv[])
{
    /* 装配参数:相机节点(sim=图片目录)/ 语言表 / 配置文件 */
#ifdef DG_SIM
    s_json_path = "configs/device.json";
    s_camera_dir = (argc > 1) ? argv[1] : "sim/media";
#else
    s_json_path = "/etc/door-guard/device.json";
    s_camera_dir = (argc > 1) ? argv[1] : "/dev/video0";
#endif
    s_ui_args.lang_dir = "ui/lang";
    s_ui_args.camera_dir = s_camera_dir;

    if (holder_init() != HOLDER_OK) {
        fprintf(stderr, "holder_init 失败\n");
        return 1;
    }
    if (register_modules() != DG_OK)
        return 1;

    /* 必需模块失败即退出(S60 监督循环 3s 拉起重试);选修失败记 ERROR 继续 */
    if (holder_init_all(true) != HOLDER_OK) {
        fprintf(stderr, "必需模块初始化失败,退出(见 holder 状态表)\n");
        return 1;
    }

    fprintf(stderr, "door-guard 运行中(Ctrl-C 退出)\n");
    for (;;) {
        camera_poll();
        ui_poll();
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return 0;
}
