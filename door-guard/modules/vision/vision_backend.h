/*
 * vision_backend.h — 视觉后端契约(把"用哪个模型/哪套推理框架"与业务服务解耦)
 *
 * 分层:
 *   vision_service(本层的服务) = 模式状态 / 特征槽 / 库维护转发 / 录入抓取,
 *      完全不认识 ROCKIVA、rknn 或任何具体模型;
 *   后端(backend)= 具体实现:ROCKIVA 官方库(默认)、自建 rknn 模型
 *      (SCRFD+ArcFace 之类开源模型)、PC mock。
 * 选择方式:编译进哪个后端由 CMake 决定(DG_SIM → sim,否则 rockiva);
 *   运行时用 cfg `face.backend` 或 env `DG_VISION_BACKEND` 按名字挑(注册表内),
 *   缺省用第一个注册的后端 —— 将来同一份固件编进多个后端可直接切,不必改服务层。
 *   装配(注册)只在 app/main.c 做(那里允许知道具体模块)。
 *
 * 后端义务(实现者逐条满足;边界都要显式处理并返回错误码):
 *  1. start() 幂等;失败返回非 0 → holder 记 ERROR,降级不影响门禁/web
 *  2. 特征:单条长度必须 ≤ DG_FEATURE_MAX(超限要在日志里明确指出并丢弃,
 *     不得静默);长度口径必须与 model_tag 一致(见下)
 *  3. compare():1=重复 / 0=不重复 / <0=错误(storage 查重契约);
 *     交给服务层注入 storage(后端不必自己调 storage_set_feature_cmp)
 *  4. lib_add/lib_del:幂等;删不存在的 id 视为成功
 *  5. 出站一律 EVENT_BUS_PUBLISH(回调在推理线程,禁止碰 LVGL/长事务)
 *  6. 发布 1:N/1:1 命中前必须查 vision_service_features_compatible():
 *     false(模型换过、旧特征未重录)时不得发布 —— 宁可不开门,不可错开门
 *  7. on_mode:进入 VERIFY_11 时按 uid 准备比对标的(如装载目标特征)
 *  8. 关键点(可选):有就按帧回灌 liveness_service_on_face(B8 活体依赖)
 *
 * 换模型的两条路(详见 modules/vision/README.md):
 *  A. 同一框架换模型文件:替换 DG_IVA_MODEL_DIR 下的 .data 文件 + 改
 *     cfg face.model_tag(第三方模型的特征空间不同,旧特征一律作废重录);
 *  B. 换框架(如 rknn 自建开源模型):新写一个 vision_xxx.c 实现本契约,
 *     在 main.c 注册;服务层/UI/FSM/存储一行不用改。
 */
#ifndef DG_VISION_BACKEND_H
#define DG_VISION_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

#include "vision_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** 后端名(cfg face.backend / env DG_VISION_BACKEND 按它匹配) */
    const char *name;
    /** 特征口径标识(模型指纹;NULL/空 = 不做口径校验,如 PC mock)。
     *  换模型必须换 tag:库里存的是旧模型特征时,新模型比对出来的分数没有意义 */
    const char *model_tag;
    /** 是否提供人脸关键点(B8 活体需要;false 时 liveness_enable=1 会被告警) */
    bool has_landmarks;

    /** 装配接线(storage 比较器/订阅/相机监听)+ 初始化;失败返回非 0 */
    int (*start)(bool enable_mock);

    /* ---- 特征库维护(转发用;可为 NULL = 该后端不支持库维护) ---- */
    int (*lib_add)(const char *user_id, const uint8_t *feature, uint16_t len);
    int (*lib_del)(const char *user_id);

    /** 查重比较器(可为 NULL = 用 storage 默认逐字节相等);
     *  签名与 storage 的 dg_feature_cmp_fn 一致,服务层直接注入 */
    int (*compare)(const uint8_t *a, uint16_t a_len,
                   const uint8_t *b, uint16_t b_len, void *ud);

    /** 工作模式变更(可为 NULL) */
    void (*on_mode)(dg_vision_mode_t mode, const char *user_id);
} vision_backend_ops_t;

/**
 * 注册后端(装配层调用;最多 4 个,重名覆盖)。
 * @return DG_OK / DG_ERR_PARAM / DG_ERR_NO_MEMORY(表满)
 */
int vision_backend_register(const vision_backend_ops_t *ops);

/** 当前生效的后端(NULL = vision_backend_start 尚未调用) */
const vision_backend_ops_t *vision_backend_active(void);

/**
 * 启动后端:按 cfg face.backend / env DG_VISION_BACKEND 选后端,
 * 接线(lib ops / compare / on_mode / 口径校验)后调 ops->start。
 * @return DG_OK 成功;非 0 = 后端不可用(缺模型等,降级不阻塞其余业务)
 */
int vision_backend_start(bool enable_mock);

/**
 * 特征口径是否与当前后端一致(模型换过且旧特征未重录 → false)。
 * 后端在发布命中前必须查;服务层启动时做登记与校验。
 */
bool vision_service_features_compatible(void);

/** 当前后端名 / 特征口径(日志、上位机展示;无后端返回 "-") */
const char *vision_backend_name(void);
const char *vision_backend_model_tag(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_VISION_BACKEND_H */
