/*
 * fp_provider.h — 指纹业务线程(FINGERPRINT_AS608.md §4)
 *
 * 持唯一业务线程:WAK 按压沿 → 采集/比对/录入序列(经链路 ops)→ 发事件。
 * 1:N/1:1 结果发布 EV_FINGER_MATCH_1N / EV_FINGER_VERIFY_11(载荷 ev_match_t,
 * 与 vision 同构,user/role 由本层反查 DB);录入进度/终态发布 EV_ENROLL_PROGRESS
 * / EV_ENROLL_RESULT(seq 与请求配对)。工作模式经 EV_FINGER_SET_MODE 切换,
 * 忙于录入/删除序列时忽略常规模式(取消 = 切 IDLE,已 Store 模板自动回滚)。
 *
 * 模组失联(open/握手连续失败)→ EV_SYS_SERVICE_STATE("finger") 降级广播,
 * 退避重试,恢复后回 READY——模组没接/串口不通整机照常(协议 §5.4)。
 */
#ifndef DG_FP_PROVIDER_H
#define DG_FP_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int fp_provider_start(void);
void fp_provider_stop(void);

/** 模组链路就绪(握手通过);验证方式选择门禁(reason=9)依据 */
bool fp_provider_ready(void);

/** registry 心跳:线程循环最近活跃时刻(ms;序列执行中按步刷新) */
int64_t fp_provider_heartbeat_ms(void);

/* ---- 链路抽象(测试注入 seam;默认 NULL = 板级 ops:uart_hal + gpio_hal) ----
 * FINGERPRINT_AS608 §7:provider 注入层 mock 替代真模组,宿主 test_fp_enroll
 * 用假模组(收指令吐脚本化应答)驱动全部序列,不碰真串口。 */
typedef struct fp_link_ops {
    int (*open)(void);                    /**< 打开链路(uart open) */
    void (*close)(void);
    int (*send)(const uint8_t *data, size_t len);
    /** 取原始字节(任意切分;无数据等待至多 timeout_ms) */
    int (*recv)(uint8_t *buf, size_t cap, int timeout_ms, size_t *out_len);
    /** 等 WAK 边沿(有沿回 DG_OK 并给新电平;超时 DG_ERR_TIMEOUT;
     *  未接线 DG_ERR_UNSUPPORTED);level 相对配置极性,本层不做极性判断 */
    int (*wak_wait)(int timeout_ms, int *level);
    /** 电平直读(消抖复核) */
    int (*wak_level)(int *level);
} fp_link_ops_t;

/** 注入链路 ops(仅测试用;须在 start 前,运行中禁止切换) */
void fp_provider_set_link_ops(const fp_link_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* DG_FP_PROVIDER_H */
