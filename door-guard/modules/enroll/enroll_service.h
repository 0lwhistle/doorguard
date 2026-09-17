/*
 * enroll_service.h — 用户/特征录入编排(architecture §1)
 *
 * 流程:EV_ENROLL_REQUEST → 视觉抓取(EV_VISION_CAPTURE_REQ →
 * EV_VISION_FEATURE 槽位回执)→ 查重(storage 比较器)→ 入库 →
 * EV_ENROLL_RESULT(错误码对齐 spec-database §2)。
 */
#ifndef DG_ENROLL_SERVICE_H
#define DG_ENROLL_SERVICE_H

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

int enroll_service_start(void);
void enroll_service_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_ENROLL_SERVICE_H */
