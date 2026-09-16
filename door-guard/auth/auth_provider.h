/*
 * auth_provider.h — 统一认证抽象接口
 *
 * 人脸(ROCKIVA)/指纹(UART 模组)/IC 卡(读头)各自实现本接口,
 * access_service 只面向本接口做认证融合,新增认证方式零改动接入。
 */
#ifndef DOOR_GUARD_AUTH_PROVIDER_H
#define DOOR_GUARD_AUTH_PROVIDER_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    AUTH_TYPE_FACE = 0,     /* ROCKIVA 人脸 */
    AUTH_TYPE_FINGER,       /* 指纹模组 */
    AUTH_TYPE_CARD,         /* IC 读卡器 */
    AUTH_TYPE_MAX
} auth_type_t;

typedef struct {
    auth_type_t type;
    uint32_t    user_id;    /* 统一用户 ID,跨认证方式关联 */
    int         score;      /* 置信度/相似度,含义由各实现定义 */
    int         ok;         /* 1=通过 0=未通过 */
} auth_result_t;

typedef struct auth_provider {
    auth_type_t type;
    const char *name;

    /* 初始化/反初始化(打开设备、加载模型等) */
    int  (*init)(const char *cfg_json);
    void (*deinit)(void);

    /* 注册:把凭证与 user_id 绑定(人脸=提特征入库,指纹=录入模组,卡=记录卡号) */
    int  (*enroll)(uint32_t user_id, const char *param);

    /* 注销 */
    int  (*abolish)(uint32_t user_id);

    /* 认证:阻塞式单次认证(人脸/活体流程由实现内部驱动) */
    int  (*verify)(auth_result_t *out, int timeout_ms);
} auth_provider_t;

/* 各实现提供的注册函数(B10 阶段逐个补充) */
auth_provider_t *auth_face_provider(void);
auth_provider_t *auth_finger_provider(void);   /* B10b */
auth_provider_t *auth_card_provider(void);     /* B10b */

#endif
