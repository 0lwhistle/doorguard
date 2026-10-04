/*
 * vision_service.h — 视觉服务(检测/特征/1:N 检索;architecture §1)
 *
 * 后端:PC=sim mock(周期事件 + 确定性伪特征);板=ROCKIVA(B7/B8 接入)。
 * 特征大数据经本模块槽位传递(事件只带 seq 句柄,架构既定的"大数据不过总线"模式)。
 * 检索范围:全部开启人脸的用户(含黑名单,命中即拒——spec-auth §2.4)。
 */
#ifndef DG_VISION_SERVICE_H
#define DG_VISION_SERVICE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "err.h"
#include "proto/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动服务(槽位/订阅) */
int vision_service_start(void);
void vision_service_stop(void);

/* ---- 工作模式(B7 交接 §0.1:FSM 状态 → vision 模式,access 1s tick 联动)
 *
 * 模式决定"帧要不要喂后端、要不要检索"——双重门禁的第二道:
 *   后端口径 = 命中阈值过滤,本层口径 = 只在允许检索的模式下检索,
 *   最终开门仍由 FSM(match_enabled/黑名单/per-user 开关)裁决。
 * access_service 经 EV_VISION_SET_MODE 下发,不直接调本模块(模块间只走总线)。
 */
typedef enum {
    DG_VMODE_IDLE = 0,      /**< 关人脸:不推帧(省 NPU;UI 预览走 RGA 通路不受影响) */
    DG_VMODE_DETECT_ONLY,   /**< 检测/画框 + 录入特征缓存,不检索(菜单/待机/结果/验证子步) */
    DG_VMODE_DETECT_1N,     /**< 上述 + 1:N 检索(普通态与管理员认证态;开机默认) */
    DG_VMODE_VERIFY_11,     /**< 与指定用户特征 1:1 比对,不检索 */
    DG_VMODE_MAX,
} dg_vision_mode_t;

/** 设工作模式(幂等;uid 仅 VERIFY_11 使用,NULL/空串=不指定)
 *  @return DG_OK / DG_ERR_PARAM(模式越界) */
int vision_service_set_mode(dg_vision_mode_t mode, const char *user_id);

/** 当前模式(后端每帧/每回调读;线程安全) */
dg_vision_mode_t vision_service_get_mode(void);

/** 模式名(日志/上位机;未知返回 "?") */
const char *vision_service_mode_name(dg_vision_mode_t mode);

/** VERIFY_11 目标用户(拷贝语义;非 VERIFY_11/未指定 → 空串) */
void vision_service_get_verify_uid(char *out, size_t cap);

/* ---- 特征库维护(后端实现,见 vision_backend.h 契约;未就绪返回 DG_ERR_NOT_INIT) ---- */
typedef int (*vision_lib_add_fn)(const char *user_id, const uint8_t *feature,
                                 uint16_t len);
typedef int (*vision_lib_del_fn)(const char *user_id);

/** 录入成功后调用(后端 INSERT 特征库) */
int vision_service_library_add(const char *user_id, const uint8_t *feature,
                               uint16_t len);
/** 用户删除后调用(后端 DELETE) */
int vision_service_library_remove(const char *user_id);

/** 注入特征库操作函数(测试替身用;NULL = 恢复后端原 ops)。
 *  装配层在后端 start 时接线;test_enroll_flow 用它模拟"库写失败"
 *  的还原路径——此前只有定义没有声明,测试只能隐式调用(告警) */
void vision_service_set_lib_ops(vision_lib_add_fn add, vision_lib_del_fn del);

/** 特征槽位:视觉后端提交(vision 内部)/ 编排侧按 seq 取(enroll 调用) */
int vision_service_submit_feature(const char *user_id, uint32_t seq,
                                  const uint8_t *feature, size_t len);
int vision_service_fetch_feature(uint32_t seq, uint8_t *out, size_t cap, size_t *len);

/* ---- 健康查询(2026-09-30 holder 健康管理推广;页面/装配层用) ---- */

/** 后端 start 成功且未被看门狗 DISABLED(识别能力在;与相机断流互相独立) */
bool vision_backend_running(void);

/** 生效后端的 worker 心跳(单调 ms;0 = 后端无心跳或未启动)——
 *  main.c 注册进 registry 作 stale 判据,worker 挂死 15s 可见 */
int64_t vision_backend_heartbeat_ms(void);

/* ---- 头像照片槽(单槽;照片是 KB 级,按架构纪律不进事件总线) ----
 *
 * 拍摄录入用:后端在 submit_feature 前先 put(同 seq 配对),
 * enroll 在 EV_VISION_FEATURE 处理时按同一 seq fetch 并落库。
 * 照片与特征必须出自同一帧——成对性由后端保证(见 vision_rknn.c 的
 * 「特征 + 同帧头像」缓存),槽位只负责按 seq 递一趟。 */

int vision_service_put_avatar(uint32_t seq, const uint8_t *jpeg, size_t len);
int vision_service_fetch_avatar(uint32_t seq, uint8_t *out, size_t cap, size_t *len);

/* ---- 静态图录入槽(单槽;web 上传 JPEG 暂存,2026-10-04) ----
 * enroll 编排在受理时 put(先于事件),后端在 EV_VISION_STILL_REQ 处理中
 * 按 seq fetch(与照片槽同款"put 先行、按 seq 递一趟"配对);新 put 顶旧槽。
 * 提取结果仍走特征槽/照片槽,失败路径 EV_VISION_STILL_FAIL。 */

int vision_service_still_put(uint32_t seq, const uint8_t *jpeg, size_t len);
int vision_service_still_fetch(uint32_t seq, uint8_t *out, size_t cap, size_t *len);

/** 当前生效后端是否支持静态图录入(enroll 受理前置;不支持 → 请求在
 *  受理时即拒绝,不会出现"受理后石沉大海") */
bool vision_service_still_supported(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_VISION_SERVICE_H */
