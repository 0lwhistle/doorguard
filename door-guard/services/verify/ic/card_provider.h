/*
 * card_provider.h — IC 读卡服务(ICCARD_PROTOCOL §7.1)
 *
 * 持唯一消费线程:poll → read → 帧校验 → HEX 卡号 → 防重窗 → EV_IC_CARD。
 * 业务判定(开门/1:1/录入)归 FSM/enroll,provider 不做任何业务分流。
 * open 失败或连续通信错 → EV_SYS_SERVICE_STATE("iccard") 降级,退避重试,
 * 恢复后回 READY——ko 不在线整机照常(协议 §10 失败隔离)。
 */
#ifndef DG_CARD_PROVIDER_H
#define DG_CARD_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

int card_provider_start(void);
void card_provider_stop(void);

/** 读卡器就绪(握手成功且在位;降级期间 false)。主页不常挂提示,
 *  只在验证方式选择时作为 reason=9 的判定依据 */
bool card_provider_ready(void);

/* ---- 防重窗(ICCARD_PROTOCOL §7.1:普通开门分支专用) ----
 * 同一卡号 window_ms 内只算一次验证,窗外/不同卡号放行。纯函数供宿主单测。 */

typedef struct {
    char    last_no[DG_IC_LEN];       /**< 窗内上一张卡(空 = 无) */
    int64_t last_ms;                  /**< 上次放行时刻(monotonic ms) */
} card_dedup_t;

bool card_dedup_allow(card_dedup_t *st, const char *no, int64_t now_ms,
                      int32_t window_ms);

#ifdef __cplusplus
}
#endif

#endif /* DG_CARD_PROVIDER_H */
