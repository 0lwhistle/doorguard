/*
 * door-guard main — 模块装配与生命周期(架构 v2 M2③)
 *
 * 装配分两张表(分层对应):
 *   holder(components/holder):基础设施与 modules 层
 *     event_bus → tasker → storage → config → relay → audio → camera
 *   registry(components/registry):services 层(依赖可跨表解析到 holder)
 *     capture → vision_service → vision_backend → access → enroll → finger
 *     → iccard → liveness → ntp → sysctl → net_cfg → web → mdns → mqtt → ui
 *
 * 初始化完成后 main 线程转看门狗(5s 巡检):
 *   - 可选服务异常/心跳超龄 → 重启一次 → 仍异常置 DISABLED + EV_SYS_SERVICE_STATE
 *     通知 UI/上位机(降级矩阵:camera/vision 挂 → 禁人脸验证,密码/指纹/IC 照常;
 *     net 挂 → web/ota/mdns/ntp 不启动,本地功能不受影响)
 *   - 必需服务(vision_service/access)不可恢复 → 安全停机(继电器复位 + 退出,
 *     S60 监督循环 3s 拉起重试)
 *
 * 模块间只经 proto/ 定义的队列与事件总线通信,不直接互调。
 * #ifdef DG_SIM 仅出现在本装配与 sim 后端(纪律允许范围)。
 */
#include "modules/camera/camera.h"
#include "modules/display/display.h"
#include "access_service.h"
#include "card_provider.h"
#include "capture_service.h"
#include "cfg.h"
#include "dg_log.h"
#include "enroll_service.h"
#include "event_bus.h"
#include "events.h"
#include "fp_provider.h"
#include "holder.h"
#include "liveness_service.h"
#include "mdns/mdns_responder.h"
#include "mqtt/mqtt_service.h"
#include "audio/audio_player.h"
#include "net/netcore.h"
#include "net/net_cfg.h"
#include "ntp/ntp_service.h"
#include "registry.h"
#include "relay.h"
#include "sysctl/sysctl_service.h"
#include "web/web_server.h"
#include "storage.h"
#include "tasker.h"
#include "timeutil.h"
#include "ui.h"
#include "vision_backend.h"
#include "vision_service.h"

#include <pthread.h>
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

/* 看门狗参数(巡检频率 5s;心跳超龄 = 连续 3 个巡检周期未刷新) */
#define WD_INTERVAL_MS 5000
#define WD_STALE_MS    (WD_INTERVAL_MS * 3)
#define WD_MAX_RESTARTS 1

/* 主循环心跳监控:registry 看门狗巡检在主循环里跑,主循环自己卡死时它
 * 一样卡死(管不了自己)。独立线程盯主循环心跳,超龄即退出进程——
 * 表现为"画面冻在某一帧"的主循环卡死,转成 3s 内被 S60 拉起重启 */
#define WD_LOOP_STALE_MS 10000
static volatile int64_t s_loop_beat_ms;
static volatile bool s_loop_started;

static void *loop_watchdog_thread(void *arg)
{
    (void)arg;
    for (;;) {
        struct timespec ts = { 2, 0 };
        nanosleep(&ts, NULL);
        if (!s_loop_started)
            continue;
        /* MONOTONIC:节拍与比较必须同时域——REALTIME 会被 NTP/RTC 校时
         * 步进,跳变后差值爆表即误判主循环卡死自杀(2026-10-03 板上
         * 时钟域连环误伤定案,健康/心跳检查一律单调时域) */
        const int64_t now = now_mono_ms();
        if (now - s_loop_beat_ms > WD_LOOP_STALE_MS) {
            DG_LOGE("[MAIN]", "主循环 %lldms 无心跳(渲染/取流卡死),退出交 S60 重拉",
                    (long long)(now - s_loop_beat_ms));
            exit(1);
        }
    }
    return NULL;
}

/* 装配参数(依赖 init_fn 无参数,故经静态变量传给包装函数) */
static const char *s_def_path;      /* 出厂模板(只读) */
static const char *s_cur_path;      /* 现用配置(首启自动生成) */
static dg_ui_args_t s_ui_args;
static const char *s_camera_dir;

/* ---- holder:基础设施与 modules 层 ---- */

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

/* 配置双文件(M2①):出厂模板 + 现用配置;板上模板在 /etc(随固件),
 * 现用配置放 /userdata 持久分区(A/B 升级换 rootfs 不丢用户设置) */
static int mod_config(void)
{
    return cfg_load(s_def_path, s_cur_path);
}

static int mod_camera(void)
{
    return camera_init(s_camera_dir, NULL, NULL);
}

/* 继电器(A4):引脚号从配置传入;宿主/硬件缺失由模块内部降级(恒 READY) */
static int mod_relay(void)
{
    return relay_module_init(cfg_get()->relay_gpio_line);
}

/* 语音播报(MAX98357;2026-10-04):后端打不开模块内部降级静默并懒重试,
 * 硬件接入后零改动恢复——不能因"没喇叭"拦门禁主链路 */
static int mod_audio(void)
{
    return audio_player_start();
}

/* ---- registry:services 层(包装函数与原 holder 版本一致) ---- */

static int mod_vision_service(void)
{
    return vision_service_start();
}

static int mod_access(void)
{
    return access_service_start();
}

static int mod_capture(void)
{
    return capture_service_start();
}

/* 视觉后端:注册进服务层注册表(契约 vision_backend.h)。
 * 编进哪个后端由 CMake 决定,装配层是唯一知道"具体是哪个"的地方(#ifdef 纪律)。
 * 失败返回非 0 → registry 置 ERROR(缺模型时降级为无检测,不阻塞其余业务;
 * 失败原因见 [VISION] 日志行) */
static int mod_vision_backend(void)
{
#ifdef DG_SIM
    /* 模拟器默认开 mock(DG_SIM_VISION=0 关);板上恒 false */
    bool enable_mock = (getenv("DG_SIM_VISION") == NULL ||
                        strcmp(getenv("DG_SIM_VISION"), "0") != 0);
    extern const vision_backend_ops_t vision_backend_sim;
    vision_backend_register(&vision_backend_sim);
    return vision_backend_start(enable_mock);
#else
    /* 注册顺序 = 优先级(缺省用第一个注册的):自组 rknn(RetinaFace 检测,
     * 识别接第二阶段)为主;ROCKIVA 保留备用——它缺模型时自行返回失败降级为
     * ERROR,不影响本条。运行时用 cfg face.backend / env DG_VISION_BACKEND 切换。 */
    extern const vision_backend_ops_t vision_backend_rknn;
    extern const vision_backend_ops_t vision_backend_rockiva;
    vision_backend_register(&vision_backend_rknn);
    vision_backend_register(&vision_backend_rockiva);
    return vision_backend_start(false);
#endif
}

static int mod_enroll(void)
{
    return enroll_service_start();
}

/* 指纹模组业务线程(2026-10-01):模组没接/串口不通由 provider 内部降级
 * (EV_SYS_SERVICE_STATE + 退避重试),注册为可选服务不阻塞整机 */
static int mod_finger(void)
{
    return fp_provider_start();
}

/* IC 读卡服务(2026-10-01):ko 未加载同样走内部降级(协议 §10 失败隔离) */
static int mod_iccard(void)
{
    return card_provider_start();
}

static int mod_liveness(void)
{
    return liveness_service_start();
}

/* 统一网络事件循环:web/OTA/NTP/mDNS 全部网络 I/O 的唯一传输层。
 * 必须先于网络服务族就绪——服务的监听注册经 netcore_post 投递进 loop */
static int mod_netcore(void)
{
    return netcore_start();
}

/* NTP 服务:订阅 EV_NET_NTP_TRIGGER(菜单/上位机按钮)+ 开机自动校正一次。
 * 必须真装配:只 include 头不初始化时 running=false,触发请求会被静默丢弃
 * (表现为"按钮没反应",不报错——曾经的坑) */
static int mod_ntp(void)
{
    return ntp_service_start();
}

static int mod_web(void)
{
    return web_server_start();
}

/* sysctl:设备重启统一执行点(设备管理页/web 经 EV_SYS_REBOOT 请求)。
 * 必须真装配:只 include 头不初始化时请求被静默丢弃(同 ntp 的坑) */
static int mod_sysctl(void)
{
    return sysctl_service_start();
}

static int mod_mdns(void)
{
    return mdns_start();
}

/* MQTT 上位机通道(2026-10-04):默认空转(cfg mqtt.enabled=0);
 * 开启后跑 netcore loop,心跳=loop 活性。远程开门默认拒(安全默认,
 * 消费端接入前 EV_MQTT_CMD 只立契约) */
static int mod_mqtt(void)
{
    return mqtt_service_start();
}

/* 网络配置装配:cfg 记的是静态地址则开机应用一次(DHCP 交给 S41dhcpcd)。
 * 阻塞数百 ms(dhcpcd 交互)发生在装配期,业务尚未起来,无影响 */
static int mod_net_cfg(void)
{
    return net_cfg_apply_saved();
}

/* UI 失败仅告警(板上 web 上位机路径照常) */
static int mod_ui(void)
{
    return ui_init(&s_ui_args);
}

/* ---- 依赖声明 ---- */

static const char *const DEP_EVENT_BUS[] = { "event_bus" };
static const char *const DEP_TASKER[]    = { "tasker" };
static const char *const DEP_STORAGE[]   = { "storage" };
static const char *const DEP_CONFIG[]    = { "config" };
static const char *const DEP_CAMERA[]    = { "config", "camera" };
static const char *const DEP_VIS_BE[]    = { "vision_service", "camera" };
static const char *const DEP_TASKER_ONLY[] = { "tasker" };
static const char *const DEP_WEB[]       = { "config", "netcore" };
static const char *const DEP_MDNS[]      = { "config", "netcore" };
static const char *const DEP_MQTT[]      = { "config", "netcore" };

/* 跨表依赖解析:服务依赖的 modules 在 holder 表(装配层桥接,registry 保持通用) */
static int dep_ready(const char *name)
{
    if (holder_is_module_ready(name))
        return 1;
    return registry_is_ready(name) ? 1 : 0;
}

static registry_err_t register_services(void)
{
    struct {
        const char *name;
        registry_start_fn fn;
        bool required;
        const char *const *deps;
        int n_deps;
        registry_heartbeat_fn hb;
    } svc[] = {
        { "capture",        mod_capture,        false, DEP_CAMERA,      1, NULL },
        { "vision_service", mod_vision_service, true,  NULL,            0, NULL },
        /* vision_backend 心跳 = rknn worker 活性(契约义务 9):worker 挂死
         * 15s 后看门狗判 stale 重启,识别静默失效从此可观测 */
        { "vision_backend", mod_vision_backend, false, DEP_VIS_BE,      2,
          vision_backend_heartbeat_ms },
        { "access",         mod_access,         true,  DEP_TASKER_ONLY, 1, NULL },
        { "enroll",         mod_enroll,         false, DEP_TASKER_ONLY, 1, NULL },
        /* 指纹/读卡:线程循环即心跳(wak 等待 200ms 粒度自醒,序列执行中
         * 按步刷新)——线程挂死 15s 会被看门狗判 stale 重启一次 */
        { "finger",         mod_finger,         false, DEP_CONFIG,      1,
          fp_provider_heartbeat_ms },
        { "iccard",         mod_iccard,         false, DEP_CONFIG,      1, NULL },
        { "liveness",       mod_liveness,       false, DEP_TASKER_ONLY, 1, NULL },
        { "ntp",            mod_ntp,            false, DEP_EVENT_BUS,   1, NULL },
        { "sysctl",         mod_sysctl,         false, DEP_EVENT_BUS,   1, NULL },
        { "net_cfg",        mod_net_cfg,        false, DEP_CONFIG,      1, NULL },
        { "web",            mod_web,            false, DEP_WEB,         2,
          web_server_heartbeat_ms },
        { "mdns",           mod_mdns,           false, DEP_MDNS,        2, NULL },
        { "mqtt",           mod_mqtt,           false, DEP_MQTT,        2,
          mqtt_service_heartbeat_ms },
        /* ui 依赖 display:display 由 ui_init 内部初始化(无独立模块),
         * 故此处只声明 config(语言/主题取 cfg) */
        { "ui",             mod_ui,             false, DEP_CONFIG,      1, NULL },
    };

    for (size_t i = 0; i < sizeof(svc) / sizeof(svc[0]); i++) {
        registry_err_t rc = registry_register(svc[i].name, svc[i].fn,
                                              svc[i].required, svc[i].deps,
                                              svc[i].n_deps, svc[i].hb, NULL);
        if (rc != REG_OK) {
            fprintf(stderr, "registry 注册 %s 失败(%d)\n", svc[i].name, rc);
            return rc;
        }
    }
    return REG_OK;
}

static int register_modules(void)
{
    struct {
        const char *name;
        holder_module_init_fn fn;
        bool required;
        const char *const *deps;
        int n_deps;
    } table[] = {
        { "event_bus", mod_event_bus, true,  NULL,          0 },
        { "tasker",    mod_tasker,    true,  DEP_EVENT_BUS, 1 },
        { "storage",   mod_storage,   true,  DEP_TASKER,    1 },
        { "config",    mod_config,    true,  DEP_STORAGE,   1 },
        { "relay",     mod_relay,     false, DEP_CONFIG,    1 },
        { "audio",     mod_audio,     false, DEP_CONFIG,    1 },
        { "camera",    mod_camera,    false, DEP_CONFIG,    1 },
        { "netcore",   mod_netcore,   false, NULL,          0 },
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

/* ---- 看门狗(M2③) ---- */

/* 服务状态变化广播:UI/上位机订阅提示降级(链路对齐 EV_NET_* 家族惯例) */
static void publish_service_state(const char *name, registry_state_t st)
{
    ev_sys_service_state_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.name, sizeof(ev.name), "%s", name);
    ev.state = (int32_t)st;
    EVENT_BUS_PUBLISH(EV_SYS_SERVICE_STATE, &ev);
}

/* 安全停机:继电器先复位到断开态(走 relay 模块,电平直设+回收引脚),
 * 退出交 S60 重拉 */
static void safe_shutdown(const char *reason)
{
    DG_LOGE("[MAIN]", "必需服务不可恢复(%s):安全停机,继电器复位", reason);
    (void)relay_reset();
    fprintf(stderr, "door-guard 必需服务不可恢复(%s),安全停机(S60 将拉起重试)\n",
            reason);
    exit(1);
}

/* storage 运行期巡检(required 模块,坏了不能重启只能上报):连续两轮
 * SELECT 1 失败才广播(单次抖动不吓人),恢复回 READY 让提示撤下 */
static bool s_storage_fault;

static void watchdog_storage_once(void)
{
    bool bad = (storage_health_check() != DG_OK);
    if (bad == s_storage_fault)
        return;
    static int fail_streak;
    if (bad) {
        if (++fail_streak < 2)
            return;
    } else {
        fail_streak = 0;
    }
    s_storage_fault = bad;
    publish_service_state("storage", bad ? REG_STATE_ERROR : REG_STATE_READY);
    DG_LOGE("[MAIN]", "存储巡检:%s", bad ? "SELECT 1 连续失败(磁盘满/介质错?)" : "恢复");
}

/* 单次巡检:异常( ERROR / 心跳超龄)→ 重启一次 → 仍异常置 DISABLED + 通知;
 * 必需服务异常 → 安全停机 */
static void watchdog_once(void)
{
    watchdog_storage_once();

    /* 服务心跳(vision worker 等)报 MONOTONIC,这里必须同域比较;
     * 旧实现 now_ms()(REALTIME)遇上校时步进,把所有健康服务误判
     * "心跳超龄"连环重启/禁用(2026-10-03 板上定案) */
    int64_t now = now_mono_ms();
    uint32_t n = registry_count();
    for (uint32_t i = 0; i < n; i++) {
        const char *name = registry_name_at(i);
        if (!name)
            continue;
        registry_state_t st = registry_state(name);
        bool stale = false;
        if (st == REG_STATE_READY) {
            int64_t hb = registry_last_heartbeat_ms(name);
            stale = (hb > 0 && now - hb > WD_STALE_MS);
        }
        if (st != REG_STATE_ERROR && !stale)
            continue;

        if (registry_is_required(name)) {
            safe_shutdown(stale ? "心跳超龄" : "启动/运行异常");
            return;                          /* 不可达 */
        }

        if (registry_restart_count(name) < WD_MAX_RESTARTS) {
            DG_LOGW("[MAIN]", "看门狗:服务 %s 异常(%s),尝试重启(%u/%u)",
                    name, stale ? "心跳超龄" : "ERROR", (unsigned)registry_restart_count(name) + 1,
                    (unsigned)WD_MAX_RESTARTS);
            registry_err_t rc = registry_restart(name);
            publish_service_state(name, rc == REG_OK ? REG_STATE_READY
                                                     : REG_STATE_ERROR);
            if (rc == REG_OK)
                continue;                    /* 重启成功,下轮复检 */
        }
        (void)registry_mark_disabled(name);
        publish_service_state(name, REG_STATE_DISABLED);
    }
}

int main(int argc, char *argv[])
{
    /* 时区:SNTP 同步的是 UTC,展示走 localtime;rootfs /etc/localtime 默认
     * UTC(=显示比北京时间慢 8 小时,2026-09-27 用户反馈"NTP 不准"的根因)。
     * POSIX TZ "CST-8" = UTC+8 无夏令时,glibc 内建解析不依赖 tzdata 文件;
     * 必须先于一切 localtime 调用(UI 时钟/web 时间/日志/UI 时间戳) */
    setenv("TZ", "CST-8", 1);

#ifndef DG_SIM
    /* 开机闪残留画面治理·前半:内核点亮屏幕远早于本进程,而 fb 内存里
     * 还留着上次关机前的最后一帧,背光内核默认亮(屏 dtsi)——首帧上屏前
     * 屏幕显示的一直是那帧旧画面。进程一起来先灭背光,把窗口内容从
     * 「旧画面」换成「黑」;首帧上屏后由主循环恢复用户亮度。sysfs 直写
     * 不依赖任何模块初始化,放最前;失败(无背光节点)仅 WARN 一次 */
    (void)display_backlight_set(0);
#endif

    /* 装配参数:相机节点(sim=图片目录)/ 语言表 / 配置双文件 */
#ifdef DG_SIM
    s_def_path = "configs/default.json";
    s_cur_path = "sim/data/cur_config.json";
    s_camera_dir = (argc > 1) ? argv[1] : "sim/media";
    s_ui_args.lang_dir = "ui/lang";         /* sim 工作目录=仓库根,相对路径成立 */
#else
    s_def_path = "/etc/door-guard/default.json";
    s_cur_path = "/userdata/doorguard/cur_config.json";
    s_camera_dir = (argc > 1) ? argv[1] : "/dev/video0";
    /* 板上进程 cwd=/(S60 直起),相对 "ui/lang" 会解析到 /ui/lang——语言表
     * 从未加载成功(2026-09-28 板上日志实锤:WARN 不存在+中英切换静默无效;
     * 中文恰是回退原文才无人察觉)。资源由 dg-deploy 直推 /root/ui/lang */
    s_ui_args.lang_dir = "/root/ui/lang";
#endif
    s_ui_args.camera_dir = s_camera_dir;

    if (holder_init() != HOLDER_OK) {
        fprintf(stderr, "holder_init 失败\n");
        return 1;
    }
    if (register_modules() != DG_OK)
        return 1;

    /* 基础设施/模块:必需失败即退出(S60 监督循环 3s 拉起重试) */
    if (holder_init_all(true) != HOLDER_OK) {
        fprintf(stderr, "必需模块初始化失败,退出(见 holder 状态表)\n");
        return 1;
    }

    /* 服务层:注册表装配;依赖经解析器跨表检查 holder 就绪状态 */
    if (registry_init() != REG_OK) {
        fprintf(stderr, "registry_init 失败\n");
        return 1;
    }
    registry_set_dep_resolver(dep_ready);
    if (register_services() != REG_OK)
        return 1;
    /* 必需服务失败同样退出(与 holder 语义一致);选修失败置 ERROR 降级运行 */
    if (registry_init_all(true) != REG_OK) {
        fprintf(stderr, "必需服务初始化失败,退出(见 registry 状态表)\n");
        return 1;
    }

    fprintf(stderr, "door-guard 运行中(看门狗巡检 %ds;Ctrl-C 退出)\n",
            WD_INTERVAL_MS / 1000);
    pthread_t wd_tid;
    if (pthread_create(&wd_tid, NULL, loop_watchdog_thread, NULL) == 0)
        pthread_detach(wd_tid);
    else
        DG_LOGW("[MAIN]", "主循环监控线程创建失败(卡死只能人工复位)");
    int64_t next_scan = now_mono_ms() + WD_INTERVAL_MS;
    s_loop_beat_ms = now_mono_ms();
    s_loop_started = true;
#ifndef DG_SIM
    bool first_frame_lit = false;
#endif
    for (;;) {
        camera_poll();
        ui_poll();
#ifndef DG_SIM
        /* 开机闪残留画面治理·后半:首轮 lv_timer_handler 已把 home 渲染
         * 并提交翻转,此刻拉亮用户亮度才不会露出 dumb buffer 未渲染内容;
         * ui 起不来则保持黑屏——黑屏(上位机可诊断)好过旧画面+死机 */
        if (!first_frame_lit && registry_state("ui") == REG_STATE_READY) {
            first_frame_lit = true;
            (void)display_backlight_set(cfg_get()->brightness);
            DG_LOGI("[MAIN]", "UI 首帧就绪,背光恢复 %d%%", cfg_get()->brightness);
        }
#endif
        s_loop_beat_ms = now_mono_ms();
        int64_t now = s_loop_beat_ms;
        if (now >= next_scan) {
            watchdog_once();
            next_scan = now + WD_INTERVAL_MS;
        }
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return 0;
}
