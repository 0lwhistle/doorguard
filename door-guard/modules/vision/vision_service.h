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
#include <stdint.h>

#include "err.h"
#include "proto/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动服务(槽位/订阅) */
int vision_service_start(void);
void vision_service_stop(void);

/** 启动后端:PC=vision_sim(mock 可关)/ 板=vision_rockiva(占位) */
void vision_backend_start(bool enable_mock);

/** 特征槽位:视觉后端提交(vision 内部)/ 编排侧按 seq 取(enroll 调用) */
int vision_service_submit_feature(const char *user_id, uint32_t seq,
                                  const uint8_t *feature, size_t len);
int vision_service_fetch_feature(uint32_t seq, uint8_t *out, size_t cap, size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* DG_VISION_SERVICE_H */
