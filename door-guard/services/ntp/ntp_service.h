/*
 * ntp_service.h — NTP 时间校正(spec-network §3)
 *
 * 三触发点:开机自动一次 / 菜单按钮 / web 按钮。
 * 联网检测:连通性探测(外网 ping);执行:chronyc burst + waitsync。
 * 结果经 EV_NET_NTP_RESULT 广播(UI 提示、web 推送、日志)。
 */
#ifndef DG_NTP_SERVICE_H
#define DG_NTP_SERVICE_H

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

int ntp_service_start(void);

/** 手动触发(菜单/web 按钮);同步阻塞执行(秒级),结果经事件广播 */
int ntp_service_trigger(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_NTP_SERVICE_H */
