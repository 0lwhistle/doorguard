/*
 * bridge.h — UI 桥接层:六层里唯一允许触碰后端的入口(学自 ESP32 ovs)
 *
 * 分层契约:
 *   pages/     纯视图,只建控件 + setter,不 include 任何后端头
 *   presenter/ 业务胶水:经 bridge 发动作/拉快照,经 navigator 切页
 *   bridge/    本层:封装 event_bus 动作发布、后端快照读取、_() 取文案
 *
 * 页面/presenter 只 include 本头;后端头(events/storage/cfg)只有
 * bridge.c 出现——后端变化只改 bridge,不动 UI。
 */
#ifndef DG_BRIDGE_H
#define DG_BRIDGE_H

#include "i18n.h"          /* _(label) 取文案(未命中回退原文) */
#include "ui_events.h"
#include "events.h"        /* ev_ui_btn_t / ev_goto_page_t 等契约结构 */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 初始化:订阅后端事件 → ui_events 队列(一次即可) ---- */
void bridge_init(void);

/* ---- 动作(→ event_bus,服务层统一决策) ---- */
void bridge_btn(const ev_ui_btn_t *btn);      /* 页面按钮请求(返回/验证…) */
void bridge_touch(void);                      /* 待机页触摸唤醒请求 */

/* ---- 数据快照(presenter 拉取式刷新用) ---- */
/* cfg 快照经 cfg_get();用户/日志列表经 storage——随页迁移逐步收口到本层 */

#ifdef __cplusplus
}
#endif

#endif /* DG_BRIDGE_H */
