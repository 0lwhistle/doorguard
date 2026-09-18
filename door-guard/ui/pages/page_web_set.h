/*
 * page_web_set.h — Web 管理页(设备管理页的子页)
 *
 * 分层契约(bridge.h 头注释):page 只建控件与 setter,不 include 任何
 * 后端头;状态由 presenter 收事件后经 setter 推给本页。
 */
#ifndef DG_PAGE_WEB_SET_H
#define DG_PAGE_WEB_SET_H

#include "lvgl.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void page_web_set_create(lv_obj_t *parent);
void page_web_set_destroy(void);

/** 展示 web 服务状态(presenter 收 EV_NET_WEB_STATE 后调用)
 *  @param running     web 服务是否在运行
 *  @param url         局域网访问地址(可空)
 *  @param user        当前账号(可空)
 *  @param pwd_default 仍是出厂默认口令
 */
void page_web_set_show(bool running, const char *url, const char *user,
                       bool pwd_default);

#ifdef __cplusplus
}
#endif

#endif /* DG_PAGE_WEB_SET_H */
