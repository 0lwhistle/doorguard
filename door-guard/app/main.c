/*
 * door-guard main — 模块装配与生命周期
 *
 * 启动顺序(故障即退出,由 systemd 拉起重试):
 *   1. event_bus / tasker(基础组件)
 *   2. storage(数据库+加密)→ cfg(默认→json→DB 覆盖)
 *   3. UI(lvgl+display+i18n+页面)
 *   4. camera(取流;失败不致命,UI 显示未就绪)
 *   5. 主循环:camera_poll + ui_poll
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


int main(int argc, char *argv[])
{
    if (event_bus_init() != EVENT_BUS_OK) {
        fprintf(stderr, "event_bus_init 失败\n");
        return 1;
    }
    if (tasker_init() != TASK_OK) {
        fprintf(stderr, "tasker_init 失败\n");
        return 1;
    }
#ifdef DG_SIM
    system("mkdir -p sim/data lang configs");   /* 模拟器工作目录(首次运行创建) */
#endif
    if (storage_init(DG_DB_PATH, DG_KEY_PATH) != DG_OK) {
        fprintf(stderr, "storage_init 失败(%s)\n", DG_DB_PATH);
        return 1;
    }

    /* 配置文件路径:板上 /etc,模拟器取仓库 configs/(开发便利) */
#ifdef DG_SIM
    const char *json = "configs/device.json";
#else
    const char *json = "/etc/door-guard/device.json";
#endif
    cfg_load(json);

    /* 服务层:取流状态 → 视觉 → 认证融合/日志 → 录入 → 活体占位 */
    capture_service_start();
    vision_service_start();
    access_service_start();
    enroll_service_start();
    liveness_service_start();
    web_server_start();
    mdns_start();

    dg_ui_args_t ui_args = {
        .lang_dir = "ui/lang",
#ifdef DG_SIM
        .camera_dir = (argc > 1) ? argv[1] : "sim/media",
#else
        .camera_dir = (argc > 1) ? argv[1] : "/dev/video0",
#endif
    };
    if (ui_init(&ui_args) != DG_OK) {
        fprintf(stderr, "ui_init 失败(板上待 Phase 8 DRM 接入)\n");
        /* 板上 UI 未就绪不退出:验证/存储仍可服务 web 上位机 */
    }

    /* camera:失败不致命(spec-auth-business §5 摄像头未就绪路径) */
    if (camera_init(ui_args.camera_dir, NULL, NULL) != DG_OK)
        fprintf(stderr, "camera_init 失败(目录 %s)\n", ui_args.camera_dir);

    /* 视觉 mock:模拟器默认开(DG_SIM_VISION=0 关);板上仅录入抓取订阅 */
#ifdef DG_SIM
    bool enable_mock = (getenv("DG_SIM_VISION") == NULL ||
                        strcmp(getenv("DG_SIM_VISION"), "0") != 0);
#else
    bool enable_mock = false;
#endif
    vision_backend_start(enable_mock);

    fprintf(stderr, "door-guard 运行中(Ctrl-C 退出)\n");
    for (;;) {
        camera_poll();
        ui_poll();
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return 0;
}
