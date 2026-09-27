/*
 * page_net_set.h — 网络配置设置页(设备端;2026-09-27 从只读展示升级为可设置)
 *
 * 分层:页面只管视图与输入弹窗;应用请求经 EV_NET_CFG_SET 发总线,
 * 结果由 presenter 经 page_net_set_on_result() 推回弹窗。
 */
#ifndef PAGE_NET_SET_H
#define PAGE_NET_SET_H

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void page_net_set_create(lv_obj_t *parent);
void page_net_set_destroy(void);

/** 实际生效地址(1s 轮询渲染) */
void page_net_set_set_addr(const char *ifname, const char *ip,
                           const char *gw, bool have_ip);

/** 应用结果回执(presenter 转发总线 EV_NET_CFG_RESULT) */
void page_net_set_on_result(bool ok, int err, const char *ip);

#ifdef __cplusplus
}
#endif

#endif /* PAGE_NET_SET_H */
