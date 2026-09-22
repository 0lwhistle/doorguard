/*
 * ntp_service.h — NTP 时间校正(spec-network §3)
 *
 * 三触发点:开机自动一次 / 菜单按钮 / web 按钮。
 * 2026-09-22 起:应用内 SNTP 客户端(mongoose,跑在 netcore 统一事件循环),
 * 替代 chrony 外部依赖;服务器地址 device_config ntp_server。
 * 联网检测:net_info_is_online();结果经 EV_NET_NTP_RESULT 广播
 * (UI 提示、web 推送、日志)。触发立即返回,执行为 loop 内异步事件。
 */
#ifndef DG_NTP_SERVICE_H
#define DG_NTP_SERVICE_H

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

int ntp_service_start(void);

/** 手动触发(菜单/web 按钮);立即返回不阻塞,结果经 EV_NET_NTP_RESULT 广播 */
int ntp_service_trigger(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_NTP_SERVICE_H */
