/*
 * web_session.h — 上位机会话(token)表(spec-network §1)
 *
 * 为什么要单独一层:token 的"容量/过期/滑动续期/吊销"是纯逻辑,
 * 抽出来可以脱离 socket 单测(注入 now),而 socket 层只做取头/校验。
 *
 * 语义:
 *   create   签发 32 hex 随机 token(TTL 1h),并**吊销其余全部会话**
 *            ——单会话策略:同一时刻只允许一个管理员在线,新登录必胜
 *   validate 校验并按需滑动续期(活跃用户不会被中途踢出)
 *   revoke   单条吊销(logout);revoke_all 全吊销(改凭据时调用)
 * 时间为入参(不内部取时钟):单测可以"快进"验证过期,无需睡一秒。
 */
#ifndef DG_WEB_SESSION_H
#define DG_WEB_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_SESSION_MAX    8      /**< 表槽位数(单会话下仅占 1 槽,防御性上限) */
#define WEB_SESSION_TTL_S  3600   /**< 会话有效期(秒) */
#define WEB_TOKEN_LEN      32     /**< hex 长度(16B 随机) */

/** 签发:token 缓冲需 WEB_TOKEN_LEN+1;expires_in_s 可传 NULL */
int web_session_create(char *out, size_t cap, int *expires_in_s);

/** 校验;now 由调用方给(time(NULL))。命中即滑动续期 */
bool web_session_validate(const char *token, time_t now);

/** 吊销单条(token 为 NULL 时等于 revoke_all) */
void web_session_revoke(const char *token);

/** 吊销全部(改凭据/停服) */
void web_session_revoke_all(void);

/** 当前有效会话数(诊断/测试) */
int web_session_count(time_t now);

#ifdef __cplusplus
}
#endif

#endif /* DG_WEB_SESSION_H */
