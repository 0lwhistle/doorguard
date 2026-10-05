/*
 * events.h — door-guard 业务事件总线契约(EV_* 全集与负载结构)
 *
 * 模块间通信只经本契约(spec-auth-business/architecture):发布方填负载,
 * 订阅方只依赖本头文件,禁止自定义重复结构。事件编码沿用 event_bus 的
 * (module_id << 16) | event_id;载荷 ≤ EVENT_BUS_MAX_EVENT_SIZE(256B,
 * 编译期 _Static_assert 逐个守卫),大数据(帧/特征)走 HAL 环形缓冲/DB,
 * 事件只传句柄与 ID。
 *
 * 方向约定(谁发布 → 谁订阅):
 *   vision/capture → access_service / UI:检测框、1:N、1:1、断流
 *   access_service → UI / web:认证结果、门控动作(验证事件唯一出口)
 *   UI → enroll_service:录入请求;enroll_service → UI:结果/进度
 *   HAL(uart/gpio)→ 服务:指纹按下、刷卡、门磁
 *   net → UI / web:联网状态、NTP 结果、OTA 进度
 */
#ifndef DG_EVENTS_H
#define DG_EVENTS_H

#include "event_bus_types.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 事件类型定义(module 见 event_bus_types.h DG_MODULE_ID_*) ---- */

#define EV_DEF(mod, id) ((event_type_t)(((mod) << 16) | (id)))

/* AUTH:认证结果与门控(access_service 唯一出口) */
#define EV_AUTH_RESULT       EV_DEF(DG_MODULE_ID_AUTH, 0x0001)  /**< 每次验证动作(成功/失败) */
#define EV_AUTH_DOOR_OPEN    EV_DEF(DG_MODULE_ID_AUTH, 0x0002)  /**< 开门指令生效 */
/* 契约状态(C1 盘点,2026-09-28):下面这些事件当前无发布方或无订阅方,
 * 契约名保留、注释标明原因;接入/取代时回填,不删号(防第三方/上位机
 * 已按 ID 对接)。详见 docs/PENDING_DECISIONS.md C1 拍板。 */
#define EV_AUTH_DOOR_CLOSE   EV_DEF(DG_MODULE_ID_AUTH, 0x0003)  /**< [死契约·被取代] 电平型继电器由 relay_door_pulse 内部拉低复位,无独立闭合事件;脉冲型继电器接入时启用 */

/* VISION:检测/匹配(vision → access/UI) */
#define EV_VISION_FACE_BOX   EV_DEF(DG_MODULE_ID_VISION, 0x0001) /**< 人脸框状态(节流) */
#define EV_VISION_FACE_LOST  EV_DEF(DG_MODULE_ID_VISION, 0x0002) /**< 人脸离开 */
#define EV_VISION_MATCH_1N   EV_DEF(DG_MODULE_ID_VISION, 0x0003) /**< 1:N 检索结果 */
#define EV_VISION_VERIFY_11  EV_DEF(DG_MODULE_ID_VISION, 0x0004) /**< 1:1 比对结果 */
#define EV_VISION_SET_MODE   EV_DEF(DG_MODULE_ID_VISION, 0x0005) /**< 工作模式切换(access→vision) */
#define EV_VISION_QUALITY    EV_DEF(DG_MODULE_ID_VISION, 0x0006) /**< 人脸质量判定(拍摄页实时提示) */

/* SYSTEM:系统/装配层(看门狗 → UI/web 上位机) */
#define EV_SYS_SERVICE_STATE EV_DEF(DG_MODULE_ID_SYSTEM, 0x0010) /**< 看门狗处置/存储巡检/relay 开门失败(2026-09-30 起有订阅:UI bridge 故障提示 + access FSM 人脸禁用) */
#define EV_SYS_REBOOT        EV_DEF(DG_MODULE_ID_SYSTEM, 0x0011) /**< 设备重启请求(ev_sys_reboot_t) */

/* CAPTURE:取流状态(capture → UI/服务) */
#define EV_CAPTURE_STATE     EV_DEF(DG_MODULE_ID_CAPTURE, 0x0001) /**< [预留] 就绪/断流;待上位机/降级提示接入 */

/* ---- ENROLL:录入编排(UI → enroll → UI) ---- */

/** EV_ENROLL_REQUEST:op 决定语义 */
typedef enum {
    DG_ENROLL_FACE = 0,                   /**< 采集人脸特征 */
    DG_ENROLL_FINGER = 1,                 /**< 采集指纹特征(两次按压) */
    DG_ENROLL_DELETE = 2,                 /**< 删除用户(含指纹模组模板级联) */
    DG_ENROLL_FACE_CLEAR = 3,             /**< 清除已录人脸(保留用户) */
    /* 2026-10-01 指纹/IC 应用层接入新增(硬件侧就绪前 UI 已按本契约施工) */
    DG_ENROLL_FINGER_CANCEL = 4,          /**< 取消指纹录入(页面退出;未存模板自动回滚) */
    DG_ENROLL_IC = 5,                     /**< 绑定 IC 卡(下一张刷入的卡生效) */
    DG_ENROLL_IC_CANCEL = 6,              /**< 取消绑卡 */
    DG_ENROLL_FINGER_DEL = 7,             /**< 删除单枚指纹(arg = page_id) */
    DG_ENROLL_IC_CLEAR = 8,               /**< 解绑 IC 卡 */
} dg_enroll_kind_t;

typedef struct {
    char    user_id[DG_UID_LEN];
    int32_t kind;                         /**< dg_enroll_kind_t */
    uint32_t seq;                         /**< 请求序号:结果回执按 seq 配对 */
    int32_t arg;                          /**< kind 专用参数(FINGER_DEL:page_id;其余 0) */
} ev_enroll_request_t;
#define EV_ENROLL_REQUEST    EV_DEF(DG_MODULE_ID_ENROLL, 0x0001) /**< 录入/删除请求 */
/* 进度事件(2026-10-01 复活,原 2026-09-28 死契约):两段式草稿场景仍由
 * EV_ENROLL_RESULT 承载(人脸),指纹两次按压的"请再次按压"中间态没有
 * 终态可替代——按原约定"接入时回填"恢复本号,语义 = 指纹录入中间提示 */
#define EV_ENROLL_PROGRESS   EV_DEF(DG_MODULE_ID_ENROLL, 0x0002) /**< 录入中间进度(指纹两次按压) */
#define EV_ENROLL_RESULT     EV_DEF(DG_MODULE_ID_ENROLL, 0x0003) /**< 终态(含错误码) */

/* NET:网络侧(net → UI/web) */
#define EV_NET_STATE         EV_DEF(DG_MODULE_ID_NET, 0x0001)    /**< [死契约·被取代] 被 EV_NET_ADDR(地址快照监视)取代 */
#define EV_NET_NTP_RESULT    EV_DEF(DG_MODULE_ID_NET, 0x0002)    /**< NTP 校正结果 */
#define EV_NET_NTP_TRIGGER   EV_DEF(DG_MODULE_ID_NET, 0x0004)    /**< 请求校正一次(菜单按钮→net) */
#define EV_NET_OTA_PROGRESS  EV_DEF(DG_MODULE_ID_NET, 0x0003)    /**< [预留] OTA 进度/终态;本打算给上位机,WS 未接 */
/* web 上位机账号管理(设备页 ↔ net):UI 不碰凭据存储,只发请求、收状态 */
#define EV_NET_WEB_STATE_REQ EV_DEF(DG_MODULE_ID_NET, 0x0005)    /**< UI→net:请回报 web 状态 */
#define EV_NET_WEB_STATE     EV_DEF(DG_MODULE_ID_NET, 0x0006)    /**< net→UI:web 运行状态快照 */
#define EV_NET_WEB_SET       EV_DEF(DG_MODULE_ID_NET, 0x0007)    /**< UI→net:改账号/口令请求 */
#define EV_NET_WEB_SET_RESULT EV_DEF(DG_MODULE_ID_NET, 0x0008)   /**< net→UI:改账号/口令结果 */
#define EV_NET_ADDR          EV_DEF(DG_MODULE_ID_NET, 0x0009)    /**< 主接口地址变化(DHCP 续租/应用静态配置) */
#define EV_NET_CFG_SET       EV_DEF(DG_MODULE_ID_NET, 0x000A)    /**< UI→net:应用网络配置(设备端屏幕设置) */
#define EV_NET_CFG_RESULT    EV_DEF(DG_MODULE_ID_NET, 0x000B)    /**< net→UI:网络配置应用结果 */
#define EV_NET_OTA_UPDATE    EV_DEF(DG_MODULE_ID_NET, 0x000C)    /**< OTA 升级状态机(公告评估/下载进度/暂存终态;services/ota/ota_update) */

/* HAL:硬件事件(HAL → 服务)。IC 已激活(2026-10-01 card_provider 落地);
 * 指纹状态/门磁链路尚未接入,契约保留待硬件接入 */
#define EV_FINGER_STATUS     EV_DEF(DG_MODULE_ID_HAL, 0x0001)    /**< [死契约·待硬件] 指纹按压/释放/错误(fp_provider 落地后激活) */
#define EV_IC_CARD           EV_DEF(DG_MODULE_ID_HAL, 0x0002)    /**< 读到卡号(已激活:card_provider 发布,access/enroll 按 FSM 状态分流) */
#define EV_DOOR_STATE        EV_DEF(DG_MODULE_ID_HAL, 0x0003)    /**< [死契约·待硬件] 门磁/门控反馈 */
/* 指纹验证结果(FINGERPRINT_AS608.md §4;2026-09-30 新增)。
 * 载荷与 vision 同构,复用 ev_match_t:matched/user_id/role/score_permille,
 * 发布侧 fp_provider,订阅侧 access_service → FSM 指纹分支(method=2) */
#define EV_FINGER_MATCH_1N   EV_DEF(DG_MODULE_ID_HAL, 0x0004)    /**< 1:N 检索结果(Search 命中→反查 DB) */
#define EV_FINGER_VERIFY_11  EV_DEF(DG_MODULE_ID_HAL, 0x0005)    /**< 1:1 验证结果(v_finger 子步) */
/* 指纹工作模式(access_service 状态派生 / enroll 编排下发 → fp_provider;
 * 2026-10-01,与 EV_VISION_SET_MODE 同构)。provider 忙于录入/删除序列时
 * 忽略 IDLE/SCAN/VERIFY 三种常规模式,防状态互踩 */
#define EV_FINGER_SET_MODE   EV_DEF(DG_MODULE_ID_HAL, 0x0006)    /**< 指纹工作模式切换 */
/* 读卡器控制(enroll 编排 → card_provider):换模式时清驱动帧缓冲与防重窗,
 * 防半秒前的旧卡串进新会话(ICCARD_PROTOCOL §4 FLUSH 时点) */
#define EV_ICCARD_CTRL       EV_DEF(DG_MODULE_ID_HAL, 0x0007)    /**< 读卡器控制命令 */

/* AUDIO:语音播报(modules/audio;MAX98357 I2S 功放 + 扬声器,硬件未接入前
 * 后端打不开自动降级为静默,业务不感知) */
#define EV_AUDIO_STATE       EV_DEF(DG_MODULE_ID_AUDIO, 0x0001) /**< 播放后端就绪/失联(降级提示用,UI 暂不消费) */

/* MQTT:上位机消息通道(services/mqtt;默认关闭,cfg mqtt.enabled 开启) */
#define EV_MQTT_STATE        EV_DEF(DG_MODULE_ID_MQTT, 0x0001)  /**< broker 连接建立/断开(断开自动退避重连) */
#define EV_MQTT_CMD          EV_DEF(DG_MODULE_ID_MQTT, 0x0002)  /**< 远程命令(接口预留:内置命令直答,业务命令待后续消费端订阅) */

/* UI:待机与页面(UI 内部页面管理用) */
#define EV_UI_STANDBY        EV_DEF(DG_MODULE_ID_UI, 0x0010)     /**< [死契约·被取代] 待机切换由 FSM 驱动 standby 页(经 EV_UI_GOTO_PAGE)+ 主页倒计时实现 */

/* ---- UI ↔ 服务请求/回执(服务层→UI 的"请弹窗/请切页",UI 只渲染不决策) ---- */
#define EV_UI_BTN            EV_DEF(DG_MODULE_ID_UI, 0x0020)     /**< 按钮:菜单/验证/返回 */
#define EV_UI_TEXT_INPUT     EV_DEF(DG_MODULE_ID_UI, 0x0021)     /**< 弹窗文本提交 */
#define EV_UI_METHOD_PICK    EV_DEF(DG_MODULE_ID_UI, 0x0022)     /**< 方式选择 */
#define EV_UI_TOUCH          EV_DEF(DG_MODULE_ID_UI, 0x0023)     /**< 任意触摸 */
#define EV_UI_GOTO_PAGE      EV_DEF(DG_MODULE_ID_UI, 0x0024)     /**< 服务请求切页 */
#define EV_UI_HINT           EV_DEF(DG_MODULE_ID_UI, 0x0025)     /**< 提示条语义 */
#define EV_UI_ASK_UID        EV_DEF(DG_MODULE_ID_UI, 0x0026)     /**< 请求弹 ID 输入框 */
#define EV_UI_INPUT_PWD      EV_DEF(DG_MODULE_ID_UI, 0x0027)     /**< 请求弹密码输入框(带 uid) */
#define EV_UI_PICK_METHOD    EV_DEF(DG_MODULE_ID_UI, 0x0028)     /**< 请求弹验证方式选择(auth_flags) */
#define EV_UI_RESULT         EV_DEF(DG_MODULE_ID_UI, 0x0029)     /**< 结果弹窗(ok/reason/用户名) */
#define EV_UI_HINT_CLEAR     EV_DEF(DG_MODULE_ID_UI, 0x002A)     /**< 清提示条 */
#define EV_UI_FACEBOX        EV_DEF(DG_MODULE_ID_UI, 0x002B)     /**< 服务侧脸框颜色(绿/红/隐藏) */
#define EV_UI_BRIGHTNESS     EV_DEF(DG_MODULE_ID_UI, 0x002C)     /**< 屏幕背光亮度设置(0~100,百分比;滑条/web) */

/* access 内部:FSM 定时器/心跳经 tasker 回注(私有;FSM 全部在总线线程驱动) */
#define EV_ACCESS_TIMER      EV_DEF(DG_MODULE_ID_AUTH, 0x0010)
#define EV_ACCESS_TICK       EV_DEF(DG_MODULE_ID_AUTH, 0x0011)

/* 视觉录入抓取(特征大数据走 vision 槽位句柄,事件只带 seq) */
#define EV_VISION_CAPTURE_REQ EV_DEF(DG_MODULE_ID_VISION, 0x0010)
#define EV_VISION_FEATURE     EV_DEF(DG_MODULE_ID_VISION, 0x0011)
/* 静态图录入(web 上传 JPEG,2026-10-04):与 CAPTURE_REQ 同形载荷(复用
 * ev_capture_req_t),后端从暂存图提取特征;收尾恰好其一——成功走
 * EV_VISION_FEATURE(特征槽),失败走 EV_VISION_STILL_FAIL(错误码)。
 * 配对契约的编排方是 enroll_service(受理制,见其头文件) */
#define EV_VISION_STILL_REQ   EV_DEF(DG_MODULE_ID_VISION, 0x0012)
#define EV_VISION_STILL_FAIL  EV_DEF(DG_MODULE_ID_VISION, 0x0013)

/* ---- 负载结构(字段与 access_logs / web 推送一致处显式注明) ---- */

/** EV_AUTH_RESULT:每次验证动作;字段 = access_logs 列 + 命中用户名 */
typedef struct {
    bool    has_user;                     /**< false = 陌生人 */
    char    user_id[DG_UID_LEN];
    char    user_name[DG_NAME_LEN];
    int32_t method;                       /**< dg_auth_method_t */
    int32_t result;                       /**< dg_auth_result_t */
    int32_t reason;                       /**< dg_auth_reason_t */
    int64_t ts;                           /**< 与落库 ts 一致(unix 秒) */
} ev_auth_result_t;

/** EV_VISION_FACE_BOX:state 决定脸框颜色(spec-ui §3.1 黄/绿/红) */
typedef enum {
    DG_BOX_DETECTED = 0,  /**< 黄 */
    DG_BOX_MATCHED  = 1,  /**< 绿 */
    DG_BOX_FAILED   = 2,  /**< 红 */
} dg_box_state_t;

typedef struct {
    int32_t x, y, w, h;                   /**< 像素矩形(摄像头帧坐标系) */
    int32_t state;                        /**< dg_box_state_t */
} ev_face_box_t;

/** EV_VISION_QUALITY:质量闸门判定(vision 后端在识别路径逐次测量;
 *  verdict 取 face_quality.h 的 face_quality_verdict_t 值。无脸时后端不发
 *  本事件,UI 用 EV_VISION_FACE_LOST 语义处理。face_px 供调试显示) */
typedef struct {
    int32_t verdict;                     /**< face_quality_verdict_t(0=合格) */
    int32_t face_px;                     /**< 人脸框较小边(像素) */
} ev_vision_quality_t;

/** EV_VISION_MATCH_1N / EV_VISION_VERIFY_11 共用;指纹 1:N/1:1 同构复用
 *  (EV_FINGER_MATCH_1N / EV_FINGER_VERIFY_11) */
typedef struct {
    bool    matched;
    char    user_id[DG_UID_LEN];
    char    user_name[DG_NAME_LEN];
    int32_t role;                         /**< dg_role_t(黑名单命中即拒,spec-auth §2) */
    uint32_t auth_flags;                  /**< 方式位(FSM 判"该方式未开启"用;
                                               vision 后端不填=0,无影响) */
    int32_t score_permille;               /**< 相似度千分比(0~1000) */
    int32_t spoof_challenge;              /**< 1 = 反欺骗判"疑似假体":命中不放行,
                                               发起多模态二次验证(2026-09-27);
                                               0 = 未质疑(未启用/分数合格/模型不可用) */
} ev_match_t;

/** EV_CAPTURE_STATE */
typedef struct {
    bool ready;                           /**< false = 断流/未就绪 */
} ev_capture_state_t;

/** EV_VISION_SET_MODE:模式枚举与语义见 vision_service.h dg_vision_mode_t
 *  (业务层不 include 视觉头也能发;uid 仅 VERIFY_11 有意义) */
typedef struct {
    int32_t mode;                         /**< dg_vision_mode_t */
    char    user_id[DG_UID_LEN];          /**< VERIFY_11:比对目标用户 */
} ev_vision_mode_t;

/** EV_ENROLL_REQUEST:op 决定语义(枚举与请求结构见上方 ENROLL 段) */

typedef struct {
    char    user_id[DG_UID_LEN];
    int32_t kind;                         /**< dg_enroll_kind_t */
    uint32_t seq;                         /**< 对应请求的 seq */
    int32_t percent;                      /**< 0~100 */
    int32_t step;                         /**< 进度语义(见 DG_ENROLL_FP_STEP_*;
                                               0 = 无细分,仅 percent) */
} ev_enroll_progress_t;

/** 指纹录入进度细分(指纹两次按压的中间态提示;文案由 UI 映射) */
typedef enum {
    DG_ENROLL_FP_STEP_NONE = 0,
    DG_ENROLL_FP_STEP_PRESS1 = 1,         /**< 请按压指纹 */
    DG_ENROLL_FP_STEP_PRESS2 = 2,         /**< 请再次按压同一手指 */
    DG_ENROLL_FP_STEP_RETRY2 = 3,         /**< 两次按压不一致,请用同一手指 */
    DG_ENROLL_FP_STEP_QUALITY = 4,        /**< 模组未读到指纹/成像差,调整手指重按 */
    DG_ENROLL_FP_STEP_LIFT = 5,           /**< 按压①已采到,请先抬起手指(2026-10-04
                                              用户口径:先提示放开,再提示第二按) */
    DG_ENROLL_FP_STEP_PROCESS = 6,        /**< 按压②已采到,合成/落库进行中,不再等
                                              按压(UI 停 5s 计时器:处理慢≠没按压) */
} dg_enroll_fp_step_t;

typedef struct {
    char    user_id[DG_UID_LEN];
    int32_t kind;                         /**< dg_enroll_kind_t */
    uint32_t seq;
    int32_t err;                          /**< dg_err_t(DG_OK 成功) */
} ev_enroll_result_t;

/** EV_NET_STATE */
typedef struct {
    bool online;                          /**< 已拿到 IP 且外网可达 */
} ev_net_state_t;

/** EV_NET_NTP_TRIGGER:请求校正一次(菜单按钮/web 触发;真实结果走 EV_NET_NTP_RESULT) */
typedef struct {
    bool manual;                          /**< true=用户手动触发 */
} ev_ntp_trigger_t;

/** EV_SYS_SERVICE_STATE:看门狗处置结果(重启/禁用),UI/上位机据此提示降级 */
typedef struct {
    char    name[32];                     /**< 服务名(registry 注册名) */
    int32_t state;                        /**< registry_state_t 值 */
    int32_t err;                          /**< 预留(0 = 无) */
} ev_sys_service_state_t;

/** EV_SYS_REBOOT:设备重启请求(设备端 UI/web → sysctl 服务;2026-09-27) */
typedef struct {
    uint32_t delay_ms;                    /**< 延迟执行:让 HTTP 回执/弹窗先落地 */
} ev_sys_reboot_t;

/** EV_NET_NTP_RESULT */
typedef struct {
    bool    ok;
    int32_t err;                          /**< 失败时 dg_err_t */
    int64_t synced_ts;                    /**< 成功时校正后的 unix 秒 */
} ev_ntp_result_t;

/** EV_NET_ADDR:主接口地址快照(变化时发布;web 推送/NTP 补同步/UI 刷新共用) */
typedef struct {
    char ifname[16];                      /**< 接口名;无主接口时为空串 */
    char ip[16];                          /**< 未拿到地址时为 "0.0.0.0" */
    char mask[16];                        /**< 同上 */
    char gw[16];                          /**< 无默认路由时为 "0.0.0.0" */
    bool have_ip;                         /**< true = 已拿到非链路本地 IPv4 */
} ev_net_addr_t;

/** EV_NET_CFG_SET:设备端屏幕发起的网络配置(与 web POST /api/network 同一落地) */
typedef struct {
    bool is_static;                       /**< false = DHCP */
    char ip[16];
    char mask[16];
    char gw[16];                          /**< 空串 = 不下发默认路由 */
} ev_net_cfg_set_t;

/** EV_NET_CFG_RESULT */
typedef struct {
    bool    ok;
    int32_t err;                          /**< 失败时 dg_err_t */
    char    ip[16];                       /**< 应用后的实际地址(展示/核对) */
} ev_net_cfg_result_t;

/** EV_NET_OTA_UPDATE:OTA 升级状态机一拍(公告评估/进度/终态统一走本事件;
 * UI 关于设备页主消费方,平台侧可见性走 mqtt ota/state 上报) */
typedef struct {
    uint8_t  state;                       /**< ota_upd_state_t(services/ota) */
    int32_t  err;                         /**< FAILED 时 dg_err_t,其余 DG_OK */
    uint32_t permille;                    /**< DOWNLOADING 进度 0~1000 */
    char     version[32];                 /**< 可用/在装版本(IDLE/QUERYING 空) */
    char     date[16];                    /**< 发布日期(公告携带,YYYY-MM-DD) */
    char     notes[128];                  /**< 更新说明(公告携带) */
} ev_ota_update_t;

/** EV_NET_OTA_PROGRESS */
typedef struct {
    uint32_t permille;                    /**< 0~1000 */
    bool     done;                        /**< true = 终态 */
    int32_t  err;                         /**< dg_err_t */
} ev_ota_progress_t;

/** EV_NET_WEB_STATE_REQ(无负载) / EV_NET_WEB_SET */
typedef struct {
    char user[32];                        /**< 新账号;空串 = 保持当前账号(仅改口令) */
    char pwd[32];                         /**< 新口令明文(仅在本进程内传递,不落盘/日志) */
} ev_web_set_t;

/** EV_NET_WEB_STATE:设备页"Web 管理"渲染用快照 */
typedef struct {
    char host[48];                        /**< mDNS 主机名(不含 .local) */
    char url[128];                        /**< 局域网访问地址(含端口,可能与 IP 并列) */
    char user[32];                        /**< 当前账号(展示用) */
    bool pwd_default;                     /**< 仍是出厂默认口令 → UI 提示尽快改 */
    bool running;                         /**< web 服务是否在运行 */
} ev_web_state_t;

/** EV_NET_WEB_SET_RESULT */
typedef struct {
    bool    ok;
    int32_t err;                          /**< 失败时 dg_err_t(如 DG_ERR_BAD_PWD) */
} ev_web_set_result_t;

/** EV_AUDIO_STATE:播放后端可用性(无音频设备 = 降级静默,不算故障) */
typedef struct {
    bool ready;
} ev_audio_state_t;

/** EV_MQTT_STATE:broker 连接状态(UI 暂不消费,web/上位机可订阅) */
typedef struct {
    bool    connected;
    int32_t err;                          /**< 断开原因(dg_err_t;连接成功 = DG_OK) */
} ev_mqtt_state_t;

/** EV_MQTT_CMD:远程命令(mqtt → 总线;name = <prefix>/cmd/ 后缀)
 *  接口预留(2026-10-04):mqtt_service 内置命令(ping/status)直答 rsp,
 *  业务命令(open 等)由后续消费端订阅本事件实现——本事件先立契约 */
typedef struct {
    char     name[24];                    /**< 命令名(主题后缀) */
    char     payload[128];                /**< 命令载荷原文(空串 = 无) */
    uint32_t id;                          /**< 应答配对(rsp 主题回执携带) */
} ev_mqtt_cmd_t;

/** EV_FINGER_STATUS */
typedef enum {
    DG_FINGER_PRESSED = 0,
    DG_FINGER_RELEASED = 1,
    DG_FINGER_ERROR = 2,
} dg_finger_status_t;

typedef struct {
    int32_t status;                       /**< dg_finger_status_t */
} ev_finger_status_t;

/** EV_IC_CARD */
typedef struct {
    char card_no[DG_IC_LEN];              /**< 卡号字符串(展示掩码前原文) */
} ev_ic_card_t;

/** EV_FINGER_SET_MODE:模式枚举与语义(access 派生常规模式,enroll 下发
 *  录入/删除;provider 忙于录入/删除序列时忽略常规模式,见 events.h 事件注) */
typedef enum {
    DG_FMODE_IDLE = 0,                    /**< 不做模组业务(WAK 只记状态事件) */
    DG_FMODE_SCAN_1N,                     /**< 1:N 检索(普通/管理员/待机) */
    DG_FMODE_VERIFY_11,                   /**< 1:1 验证(user_id 生效) */
    DG_FMODE_ENROLL,                      /**< 录入(两次按压,进度/结果经 ENROLL 事件) */
    DG_FMODE_FINGER_DEL,                  /**< 删单枚模板(arg = page_id) */
    DG_FMODE_DELETE_USER,                 /**< 删用户全部模板(pages[] 生效) */
} dg_finger_mode_t;

#define DG_FINGER_PAGES_MAX 3             /**< 每用户指纹上限(FINGERPRINT_AS608 决策 B) */

typedef struct {
    int32_t  mode;                        /**< dg_finger_mode_t */
    char     user_id[DG_UID_LEN];         /**< ENROLL/VERIFY_11:目标用户 */
    int32_t  arg;                         /**< FINGER_DEL:page_id;其余 0 */
    uint16_t pages[DG_FINGER_PAGES_MAX];  /**< DELETE_USER:待删 PageID 列表 */
    uint16_t page_cnt;                    /**< 上列有效个数 */
    uint32_t seq;                         /**< ENROLL 请求 seq(结果回执配对) */
} ev_finger_mode_t;

/** EV_ICCARD_CTRL:op 见 dg_iccard_ctrl_t */
typedef enum {
    DG_ICCARD_CTRL_FLUSH = 1,             /**< 清驱动帧缓冲+应用防重窗(换会话) */
} dg_iccard_ctrl_t;

typedef struct {
    int32_t op;                           /**< dg_iccard_ctrl_t */
} ev_iccard_ctrl_t;

/** EV_DOOR_STATE */
typedef struct {
    bool open;                            /**< true = 门处于开启 */
} ev_door_state_t;

/** EV_UI_STANDBY */
typedef struct {
    bool enter;                           /**< true = 进入待机;false = 唤醒 */
} ev_standby_t;

/* ---- UI ↔ 服务载荷 ---- */

typedef enum {
    DG_BTN_MENU = 0,                      /**< 主页"菜单" */
    DG_BTN_VERIFY,                        /**< 主页"验证" */
    DG_BTN_BACK,                          /**< 返回 */
} dg_ui_btn_t;

typedef struct {
    int32_t btn;                          /**< dg_ui_btn_t */
} ev_ui_btn_t;

typedef enum {
    DG_INPUT_UID = 0,                     /**< 用户 ID(验证流程) */
    DG_INPUT_PWD,                         /**< ID+密码验证 */
    DG_INPUT_TEXT,                        /**< 通用文本(用户管理等) */
} dg_input_kind_t;

typedef struct {
    int32_t kind;                         /**< dg_input_kind_t */
    char    uid[DG_UID_LEN];              /**< 密码验证时的目标用户 */
    char    text[64];
} ev_text_input_t;

typedef struct {
    int32_t method;                       /**< dg_auth_method_t */
} ev_method_pick_t;

typedef struct {
    char    page[16];                     /**< "home"/"menu"/"standby" */
} ev_goto_page_t;

/** EV_UI_BRIGHTNESS:背光亮度(0~100;执行=bridge 订阅后直写 display 原语,
 *  持久化=发布方自行 cfg_set——生效与落盘两条通道解耦) */
typedef struct {
    int32_t pct;                          /**< 0~100,越界由 display 原语钳制 */
} ev_brightness_t;

/** EV_UI_HINT:提示条文案语义
 *  method >= 0:验证方式提示(_(「请正对摄像头/请按指纹/请输入密码/请刷卡」))
 *  method <  0:非验证方式的一次性 UI 语义(见 DG_HINT_*) */
typedef struct {
    int32_t method;                       /**< -1/-2 见 DG_HINT_*;>=0 = dg_auth_method_t */
} ev_hint_t;

/* ev_hint_t.method 负值哨兵(纯 UI 文案选择,不落日志、不进 FSM) */
#define DG_HINT_ADMIN_AUTH  (-1)          /**< 「管理员认证」 */
#define DG_HINT_NO_ADMIN    (-2)          /**< 「未设置管理员,请先添加管理员」(新机引导) */
#define DG_HINT_CHALLENGE   (-3)          /**< 「人脸验证未通过,请选择其他方式」(反欺骗降级) */
/* 语言热切换不走 hint:入口在 UI 自身(page_device→i18n_set_language),
 * 直接异步 navigator_reload(2026-09-28;原 -5 哨兵从未有发布者) */

/** EV_UI_ASK_UID:请求弹 ID 输入框(无载荷) */

/** EV_UI_INPUT_PWD:请求弹密码输入框;uid 由 UI 原样回填到 EV_UI_TEXT_INPUT */
typedef struct {
    char uid[DG_UID_LEN];
} ev_ui_input_req_t;

/** EV_UI_PICK_METHOD:请求弹"验证方式"选择框;auth_flags = DG_AUTH_* 位或 */
typedef struct {
    uint32_t auth_flags;
} ev_ui_methods_t;

/** EV_UI_RESULT:结果弹窗(文案由 UI 按 reason 映射,不打印内部原因码) */
typedef struct {
    bool    ok;
    int32_t reason;                       /**< dg_auth_reason_t(ok 时 0) */
    bool    not_admin;                    /**< 管理员入口专用:提示「非管理员」 */
    char    user_name[DG_NAME_LEN];       /**< 成功弹窗显示用 */
    int32_t method;                       /**< 语境方式(dg_auth_method_t):reason=9
                                               文案按方式区分(2026-10-01 增补;
                                               -1 = 无语境,UI 取默认文案) */
} ev_ui_result_t;

/** EV_UI_FACEBOX:服务侧脸框操作;state=-1 → 隐藏;w=0 → 只改颜色不挪位置 */
typedef struct {
    int32_t state;                        /**< dg_box_state_t;-1 = 隐藏 */
    int32_t x, y, w, h;                   /**< 像素矩形;w=0 表示沿用现有位置 */
} ev_ui_facebox_t;

/** EV_UI_HINT_CLEAR:清提示条(无载荷) */

typedef struct {
    char    user_id[DG_UID_LEN];
    uint32_t seq;                         /**< 特征槽位句柄(大数据不过总线) */
    uint16_t len;                         /**< 特征明文字节数 */
} ev_feature_t;

typedef struct {
    char    user_id[DG_UID_LEN];
    uint32_t seq;
} ev_capture_req_t;

/** EV_VISION_STILL_FAIL:静态图录入失败(错误码即用户文案依据) */
typedef struct {
    char    user_id[DG_UID_LEN];
    uint32_t seq;
    int32_t err;                          /**< dg_err_t(-39/-40/-41 或通用码) */
} ev_still_fail_t;

typedef struct {
    int32_t timer_id;                     /**< fsm_timer_t */
    uint32_t seq;
} ev_access_timer_t;

/* ---- 载荷尺寸守卫:任何负载不得超过总线单事件上限 ---- */

_Static_assert(sizeof(ev_auth_result_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_auth_result_t 超限");
_Static_assert(sizeof(ev_face_box_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_face_box_t 超限");
_Static_assert(sizeof(ev_vision_quality_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_vision_quality_t 超限");
_Static_assert(sizeof(ev_match_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_match_t 超限");
_Static_assert(sizeof(ev_vision_mode_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_vision_mode_t 超限");
_Static_assert(sizeof(ev_capture_state_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_capture_state_t 超限");
_Static_assert(sizeof(ev_enroll_request_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_enroll_request_t 超限");
_Static_assert(sizeof(ev_enroll_progress_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_enroll_progress_t 超限");
_Static_assert(sizeof(ev_enroll_result_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_enroll_result_t 超限");
_Static_assert(sizeof(ev_net_state_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_net_state_t 超限");
_Static_assert(sizeof(ev_ntp_trigger_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_ntp_trigger_t 超限");
_Static_assert(sizeof(ev_ntp_result_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_ntp_result_t 超限");
_Static_assert(sizeof(ev_net_addr_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_net_addr_t 超限");
_Static_assert(sizeof(ev_net_cfg_set_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_net_cfg_set_t 超限");
_Static_assert(sizeof(ev_net_cfg_result_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_net_cfg_result_t 超限");
_Static_assert(sizeof(ev_ota_progress_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_ota_progress_t 超限");
_Static_assert(sizeof(ev_ota_update_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_ota_update_t 超限");
_Static_assert(sizeof(ev_web_set_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_web_set_t 超限");
_Static_assert(sizeof(ev_web_state_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_web_state_t 超限");
_Static_assert(sizeof(ev_web_set_result_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_web_set_result_t 超限");
_Static_assert(sizeof(ev_sys_service_state_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_sys_service_state_t 超限");
_Static_assert(sizeof(ev_sys_reboot_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_sys_reboot_t 超限");
_Static_assert(sizeof(ev_finger_status_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_finger_status_t 超限");
_Static_assert(sizeof(ev_ic_card_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_ic_card_t 超限");
_Static_assert(sizeof(ev_finger_mode_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_finger_mode_t 超限");
_Static_assert(sizeof(ev_iccard_ctrl_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_iccard_ctrl_t 超限");
_Static_assert(sizeof(ev_door_state_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_door_state_t 超限");
_Static_assert(sizeof(ev_standby_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_standby_t 超限");
_Static_assert(sizeof(ev_ui_btn_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_ui_btn_t 超限");
_Static_assert(sizeof(ev_text_input_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_text_input_t 超限");
_Static_assert(sizeof(ev_method_pick_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_method_pick_t 超限");
_Static_assert(sizeof(ev_goto_page_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_goto_page_t 超限");
_Static_assert(sizeof(ev_hint_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_hint_t 超限");
_Static_assert(sizeof(ev_ui_input_req_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_ui_input_req_t 超限");
_Static_assert(sizeof(ev_ui_methods_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_ui_methods_t 超限");
_Static_assert(sizeof(ev_ui_result_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_ui_result_t 超限");
_Static_assert(sizeof(ev_ui_facebox_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_ui_facebox_t 超限");
_Static_assert(sizeof(ev_feature_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_feature_t 超限");
_Static_assert(sizeof(ev_capture_req_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_capture_req_t 超限");
_Static_assert(sizeof(ev_still_fail_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_still_fail_t 超限");
_Static_assert(sizeof(ev_access_timer_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_access_timer_t 超限");

/** 事件名(EV_* 优先,回退 event_bus 内置名);日志/web 用 */
const char *dg_event_name(event_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* DG_EVENTS_H */
