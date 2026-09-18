/*
 * web_auth.h — 上位机凭据与登录风控(spec-network §1)
 *
 * 凭据存 device_config(web_user / web_pwd_salt / web_pwd_hash /
 * web_pwd_default),口令同款 PBKDF2-HMAC-SHA256(spec-database §3),
 * **不存明文、不落日志**。
 *
 * 账号/口令的合法性规则与设备端用户同源(proto/valid.h 唯一权威):
 * 一条规则两处执行(设备菜单改密 / 上位机改密)才不会漂移。
 *
 * 登录风控:同一来源连续失败 N 次锁定 M 秒(防局域网爆破;次数/时长
 * 取 configs 默认,见 web_auth.c 常量)。
 */
#ifndef DG_WEB_AUTH_H
#define DG_WEB_AUTH_H

#include <stdbool.h>
#include <stddef.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 账号名上限(含 0;与 proto/valid.h 的 user_id 规则一致:3~31) */
#define WEB_AUTH_USER_MAX 32
#define WEB_AUTH_IP_MAX   46

/**
 * 确保凭据存在:首次调用(新机)生成默认口令 admin/admin,并置
 * web_pwd_default=1,由 UI/上位机提示"请尽快修改"。幂等。
 * 返回 DG_OK / DG_ERR_IO(DB 不可用)。
 */
int web_auth_ensure(void);

/** 校验账号口令:成功 DG_OK;DG_ERR_WRONG_PASSWORD(含账号不符,不区分
 *  以便不泄露账号是否存在);参数非法 DG_ERR_PARAM */
int web_auth_verify(const char *user, const char *pwd);

/** 当前账号名(无则写空串) */
int web_auth_get_user(char *out, size_t cap);

/** 是否仍为出厂默认口令(true 时上位机/设备页显示安全提示) */
bool web_auth_is_default(void);

/**
 * 改账号+口令:合法性走 proto/valid.h 同一份规则(账号→uid 规则,
 * 口令→pwd 规则)。成功后**吊销全部会话**(改密即踢下线,防止旧 token
 * 继续可用)。
 * 返回 DG_OK / DG_ERR_BAD_UID / DG_ERR_BAD_PWD / DG_ERR_IO
 */
int web_auth_set(const char *user, const char *pwd);

/** 改口令(需旧口令正确);成功后吊销全部会话 */
int web_auth_change_pwd(const char *old_pwd, const char *new_pwd);

/* ---- 登录风控 ---- */

/** 该来源是否锁定中;retry_after_s 为剩余秒数(可传 NULL) */
bool web_auth_login_blocked(const char *ip, int *retry_after_s);

/** 记一次登录失败;返回 true = 本次失败触发了锁定 */
bool web_auth_login_fail(const char *ip);

/** 登录成功:清零该来源计数 */
void web_auth_login_ok(const char *ip);

#ifdef __cplusplus
}
#endif

#endif /* DG_WEB_AUTH_H */
