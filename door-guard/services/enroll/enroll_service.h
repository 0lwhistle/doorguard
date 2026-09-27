/*
 * enroll_service.h — 用户/特征录入编排(architecture §1)
 *
 * 流程(2026-09-27 起为**两段式草稿**):EV_ENROLL_REQUEST → 视觉抓取
 * (EV_VISION_CAPTURE_REQ → EV_VISION_FEATURE 槽位回执)→ 特征与头像
 * **暂存服务内草稿槽,不落库**(UI 即时预览,编辑页「保存」才提交)→
 * EV_ENROLL_RESULT(OK = 草稿就绪)。落库时机由 UI 决定:
 *   commit_draft  = 保存(查重+DB+特征库+头像一次完成)
 *   discard_draft = 放弃(安全擦除)
 * 错误码对齐 spec-database §2。
 */
#ifndef DG_ENROLL_SERVICE_H
#define DG_ENROLL_SERVICE_H

#include "err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int enroll_service_start(void);
void enroll_service_stop(void);

/* ---- 人脸草稿(单槽;UI 线程直调,登记见 docs/architecture-v2-proposal §1) ---- */

/** 该用户是否有人脸草稿(已采集未保存) */
bool enroll_service_draft_active(const char *user_id);

/** 取草稿头像 JPEG 明文指针(仅 LVGL 线程在拍摄/编辑页内使用,取用期间
 *  不得并发 commit/discard;无草稿返回 false)
 *  @return true = *jpeg 与 *len 有效(内部静态缓冲,下次采集前稳定) */
bool enroll_service_draft_avatar(const char *user_id, const uint8_t **jpeg,
                                 size_t *len);

/** 提交草稿:查重 → 特征落库 → 特征库 INSERT → 头像落库。
 *  @return DG_OK 成功(草稿已消费);失败码(DUP_FACE/DB 等)时草稿保留,
 *          由 UI 决定重拍(覆盖)或放弃(discard) */
int enroll_service_commit_draft(const char *user_id);

/** 丢弃草稿(明文安全擦除);无草稿时静默成功 */
void enroll_service_discard_draft(const char *user_id);

/** 清除已录人脸(保留用户;DB len=0 + 特征库 DELETE + 头像清空)。
 *  同步版,供 UI「保存」路径直调;web 侧仍走 DG_ENROLL_FACE_CLEAR 事件 */
int enroll_service_clear_face(const char *user_id);

#ifdef __cplusplus
}
#endif

#endif /* DG_ENROLL_SERVICE_H */
