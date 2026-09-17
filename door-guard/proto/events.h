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

/* CAPTURE:取流状态(capture → UI/服务) */
#define EV_CAPTURE_STATE     EV_DEF(DG_MODULE_ID_CAPTURE, 0x0001) /**< 就绪/断流 */

/* ENROLL:录入编排(UI → enroll → UI) */
#define EV_ENROLL_REQUEST    EV_DEF(DG_MODULE_ID_ENROLL, 0x0001) /**< 录入/删除请求 */
#define EV_ENROLL_PROGRESS   EV_DEF(DG_MODULE_ID_ENROLL, 0x0002) /**< 采集进度 */
#define EV_ENROLL_RESULT     EV_DEF(DG_MODULE_ID_ENROLL, 0x0003) /**< 终态(含错误码) */

/* NET:网络侧(net → UI/web) */
#define EV_NET_STATE         EV_DEF(DG_MODULE_ID_NET, 0x0001)    /**< 联网状态变化 */
#define EV_NET_NTP_RESULT    EV_DEF(DG_MODULE_ID_NET, 0x0002)    /**< NTP 校正结果 */
#define EV_NET_OTA_PROGRESS  EV_DEF(DG_MODULE_ID_NET, 0x0003)    /**< OTA 进度/终态 */

/* HAL:硬件事件(HAL → 服务) */
#define EV_FINGER_STATUS     EV_DEF(DG_MODULE_ID_HAL, 0x0001)    /**< 指纹按压/释放/错误 */
#define EV_IC_CARD           EV_DEF(DG_MODULE_ID_HAL, 0x0002)    /**< 读到卡号 */
#define EV_DOOR_STATE        EV_DEF(DG_MODULE_ID_HAL, 0x0003)    /**< 门磁/门控反馈 */

/* UI:待机(UI 内部页面管理用) */
#define EV_UI_STANDBY        EV_DEF(DG_MODULE_ID_UI, 0x0010)     /**< 进入/退出待机 */

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
    int32_t score_permille;               /**< 相似度千分比(0~1000) */
} ev_match_t;

/** EV_CAPTURE_STATE */
typedef struct {
    bool ready;                           /**< false = 断流/未就绪 */
} ev_capture_state_t;

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

/* ---- 载荷尺寸守卫:任何负载不得超过总线单事件上限 ---- */

_Static_assert(sizeof(ev_auth_result_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_auth_result_t 超限");
_Static_assert(sizeof(ev_face_box_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_face_box_t 超限");
_Static_assert(sizeof(ev_match_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_match_t 超限");
_Static_assert(sizeof(ev_capture_state_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_capture_state_t 超限");
_Static_assert(sizeof(ev_enroll_request_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_enroll_request_t 超限");
_Static_assert(sizeof(ev_enroll_progress_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_enroll_progress_t 超限");
_Static_assert(sizeof(ev_enroll_result_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_enroll_result_t 超限");
_Static_assert(sizeof(ev_net_state_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_net_state_t 超限");
_Static_assert(sizeof(ev_ntp_result_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_ntp_result_t 超限");
_Static_assert(sizeof(ev_ota_progress_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_ota_progress_t 超限");
_Static_assert(sizeof(ev_finger_status_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_finger_status_t 超限");
_Static_assert(sizeof(ev_ic_card_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_ic_card_t 超限");
_Static_assert(sizeof(ev_door_state_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_door_state_t 超限");
_Static_assert(sizeof(ev_standby_t) <= EVENT_BUS_MAX_EVENT_SIZE, "ev_standby_t 超限");

/** 事件名(EV_* 优先,回退 event_bus 内置名);日志/web 用 */
const char *dg_event_name(event_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* DG_EVENTS_H */
