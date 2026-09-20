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
#define EV_AUTH_DOOR_CLOSE   EV_DEF(DG_MODULE_ID_AUTH, 0x0003)  /**< 开门时长到,门已闭合 */

/* VISION:检测/匹配(vision → access/UI) */
#define EV_VISION_FACE_BOX   EV_DEF(DG_MODULE_ID_VISION, 0x0001) /**< 人脸框状态(节流) */
#define EV_VISION_FACE_LOST  EV_DEF(DG_MODULE_ID_VISION, 0x0002) /**< 人脸离开 */
#define EV_VISION_MATCH_1N   EV_DEF(DG_MODULE_ID_VISION, 0x0003) /**< 1:N 检索结果 */
#define EV_VISION_VERIFY_11  EV_DEF(DG_MODULE_ID_VISION, 0x0004) /**< 1:1 比对结果 */
#define EV_VISION_SET_MODE   EV_DEF(DG_MODULE_ID_VISION, 0x0005) /**< 工作模式切换(access→vision) */

/* SYSTEM:系统/装配层(看门狗 → UI/web 上位机) */
#define EV_SYS_SERVICE_STATE EV_DEF(DG_MODULE_ID_SYSTEM, 0x0010) /**< 服务状态变化(降级/禁用通知) */

/* CAPTURE:取流状态(capture → UI/服务) */
#define EV_CAPTURE_STATE     EV_DEF(DG_MODULE_ID_CAPTURE, 0x0001) /**< 就绪/断流 */

/* ENROLL:录入编排(UI → enroll → UI) */
#define EV_ENROLL_REQUEST    EV_DEF(DG_MODULE_ID_ENROLL, 0x0001) /**< 录入/删除请求 */
#define EV_ENROLL_PROGRESS   EV_DEF(DG_MODULE_ID_ENROLL, 0x0002) /**< 采集进度 */
#define EV_ENROLL_RESULT     EV_DEF(DG_MODULE_ID_ENROLL, 0x0003) /**< 终态(含错误码) */

/* NET:网络侧(net → UI/web) */
#define EV_NET_STATE         EV_DEF(DG_MODULE_ID_NET, 0x0001)    /**< 联网状态变化 */
#define EV_NET_NTP_RESULT    EV_DEF(DG_MODULE_ID_NET, 0x0002)    /**< NTP 校正结果 */
#define EV_NET_NTP_TRIGGER   EV_DEF(DG_MODULE_ID_NET, 0x0004)    /**< 请求校正一次(菜单按钮→net) */
#define EV_NET_OTA_PROGRESS  EV_DEF(DG_MODULE_ID_NET, 0x0003)    /**< OTA 进度/终态 */
/* web 上位机账号管理(设备页 ↔ net):UI 不碰凭据存储,只发请求、收状态 */
#define EV_NET_WEB_STATE_REQ EV_DEF(DG_MODULE_ID_NET, 0x0005)    /**< UI→net:请回报 web 状态 */
#define EV_NET_WEB_STATE     EV_DEF(DG_MODULE_ID_NET, 0x0006)    /**< net→UI:web 运行状态快照 */
#define EV_NET_WEB_SET       EV_DEF(DG_MODULE_ID_NET, 0x0007)    /**< UI→net:改账号/口令请求 */
#define EV_NET_WEB_SET_RESULT EV_DEF(DG_MODULE_ID_NET, 0x0008)   /**< net→UI:改账号/口令结果 */

/* HAL:硬件事件(HAL → 服务) */
#define EV_FINGER_STATUS     EV_DEF(DG_MODULE_ID_HAL, 0x0001)    /**< 指纹按压/释放/错误 */
#define EV_IC_CARD           EV_DEF(DG_MODULE_ID_HAL, 0x0002)    /**< 读到卡号 */
#define EV_DOOR_STATE        EV_DEF(DG_MODULE_ID_HAL, 0x0003)    /**< 门磁/门控反馈 */

/* UI:待机与页面(UI 内部页面管理用) */
#define EV_UI_STANDBY        EV_DEF(DG_MODULE_ID_UI, 0x0010)     /**< 进入/退出待机 */

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

/* access 内部:FSM 定时器/心跳经 tasker 回注(私有;FSM 全部在总线线程驱动) */
#define EV_ACCESS_TIMER      EV_DEF(DG_MODULE_ID_AUTH, 0x0010)
#define EV_ACCESS_TICK       EV_DEF(DG_MODULE_ID_AUTH, 0x0011)

/* 视觉录入抓取(特征大数据走 vision 槽位句柄,事件只带 seq) */
#define EV_VISION_CAPTURE_REQ EV_DEF(DG_MODULE_ID_VISION, 0x0010)
#define EV_VISION_FEATURE     EV_DEF(DG_MODULE_ID_VISION, 0x0011)

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

/** EV_VISION_MATCH_1N / EV_VISION_VERIFY_11 共用 */
typedef struct {
    bool    matched;
    char    user_id[DG_UID_LEN];
    char    user_name[DG_NAME_LEN];
    int32_t role;                         /**< dg_role_t(黑名单命中即拒,spec-auth §2) */
    int32_t score_permille;               /**< 相似度千分比(0~1000) */
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

/** EV_ENROLL_REQUEST:op 决定语义 */
typedef enum {
    DG_ENROLL_FACE = 0,                   /**< 采集人脸特征 */
    DG_ENROLL_FINGER = 1,                 /**< 采集指纹特征 */
    DG_ENROLL_DELETE = 2,                 /**< 删除用户 */
} dg_enroll_kind_t;

typedef struct {
    char    user_id[DG_UID_LEN];
    int32_t kind;                         /**< dg_enroll_kind_t */
    uint32_t seq;                         /**< 请求序号:结果回执按 seq 配对 */
} ev_enroll_request_t;

typedef struct {
    char    user_id[DG_UID_LEN];
    int32_t kind;                         /**< dg_enroll_kind_t */
    uint32_t seq;                         /**< 对应请求的 seq */
    int32_t percent;                      /**< 0~100 */
} ev_enroll_progress_t;

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

/** EV_NET_NTP_RESULT */
typedef struct {
    bool    ok;
    int32_t err;                          /**< 失败时 dg_err_t */
    int64_t synced_ts;                    /**< 成功时校正后的 unix 秒 */
} ev_ntp_result_t;

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

/** EV_UI_HINT:提示条文案语义
 *  method >= 0:验证方式提示(_(「请正对摄像头/请按指纹/请输入密码/请刷卡」))
 *  method <  0:非验证方式的一次性 UI 语义(见 DG_HINT_*) */
typedef struct {
    int32_t method;                       /**< -1/-2 见 DG_HINT_*;>=0 = dg_auth_method_t */
} ev_hint_t;

/* ev_hint_t.method 负值哨兵(纯 UI 文案选择,不落日志、不进 FSM) */
#define DG_HINT_ADMIN_AUTH  (-1)          /**< 「管理员认证」 */
#define DG_HINT_NO_ADMIN    (-2)          /**< 「未设置管理员,请先添加管理员」(新机引导) */
#define DG_HINT_LANG_RELOAD (-5)          /**< 语言热切换:整页重建(ui/README §5) */

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

typedef struct {
    int32_t timer_id;                     /**< fsm_timer_t */
    uint32_t seq;
} ev_access_timer_t;

/* ---- 载荷尺寸守卫:任何负载不得超过总线单事件上限 ---- */

_Static_assert(sizeof(ev_auth_result_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_auth_result_t 超限");
_Static_assert(sizeof(ev_face_box_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_face_box_t 超限");
_Static_assert(sizeof(ev_match_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_match_t 超限");
_Static_assert(sizeof(ev_vision_mode_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_vision_mode_t 超限");
_Static_assert(sizeof(ev_capture_state_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_capture_state_t 超限");
_Static_assert(sizeof(ev_enroll_request_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_enroll_request_t 超限");
_Static_assert(sizeof(ev_enroll_progress_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_enroll_progress_t 超限");
_Static_assert(sizeof(ev_enroll_result_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_enroll_result_t 超限");
_Static_assert(sizeof(ev_net_state_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_net_state_t 超限");
_Static_assert(sizeof(ev_ntp_trigger_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_ntp_trigger_t 超限");
_Static_assert(sizeof(ev_ntp_result_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_ntp_result_t 超限");
_Static_assert(sizeof(ev_ota_progress_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_ota_progress_t 超限");
_Static_assert(sizeof(ev_web_set_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_web_set_t 超限");
_Static_assert(sizeof(ev_web_state_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_web_state_t 超限");
_Static_assert(sizeof(ev_web_set_result_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_web_set_result_t 超限");
_Static_assert(sizeof(ev_sys_service_state_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_sys_service_state_t 超限");
_Static_assert(sizeof(ev_finger_status_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_finger_status_t 超限");
_Static_assert(sizeof(ev_ic_card_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_ic_card_t 超限");
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
_Static_assert(sizeof(ev_access_timer_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_access_timer_t 超限");

/** 事件名(EV_* 优先,回退 event_bus 内置名);日志/web 用 */
const char *dg_event_name(event_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* DG_EVENTS_H */
