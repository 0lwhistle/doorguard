/*
 * test_web_auth.c — web 上位机凭据 / 会话 / 登录风控(web_auth.h + web_session.h)
 *
 * 这几处直接决定"谁能登进上位机",必须脱离 HTTP 层测:
 *   - 首启凭据自动生成 + 默认口令标记(UI 据此提示尽快改密)
 *   - 改账号/口令的合法性走 proto/valid 同一份规则(非法必须被拒)
 *   - **改凭据即吊销全部会话**(旧 token 不能继续用)
 *   - 会话表:签发/校验/过期(注入 now 快进)/注销/表满不丢新 token
 *   - 登录风控:N 次失败锁定、锁定期计数清零、成功即解锁
 */
#include "dg_test.h"
#include "err.h"
#include "storage.h"
#include "web_auth.h"
#include "web_session.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TMP_DB  "/tmp/dg_webauth_test.db"
#define TMP_KEY "/tmp/dg_webauth_test.key"

static void fresh_storage(void)
{
    storage_deinit();
    remove(TMP_DB);
    remove(TMP_KEY);
    DG_CHECK(storage_init(TMP_DB, TMP_KEY) == DG_OK);
}

static void t_credentials(void)
{
    printf("[WA1] 首启生成默认凭据 + 改账号/口令\n");
    char user[WEB_AUTH_USER_MAX] = "";
    DG_CHECK(web_auth_ensure() == DG_OK);
    DG_CHECK(web_auth_is_default() == true);          /* 首启:默认口令 */
    DG_CHECK(web_auth_get_user(user, sizeof(user)) == DG_OK);
    DG_CHECK(strcmp(user, "admin") == 0);

    /* 默认口令可登录 */
    DG_CHECK(web_auth_verify("admin", "admin") == DG_OK);
    /* 口令错 / 账号错:都归为"验证失败"(不泄露账号是否存在) */
    DG_CHECK(web_auth_verify("admin", "wrong") == DG_ERR_WRONG_PASSWORD);
    DG_CHECK(web_auth_verify("root", "admin") == DG_ERR_WRONG_PASSWORD);
    DG_CHECK(web_auth_verify(NULL, "admin") == DG_ERR_PARAM);

    /* 改账号+口令:合法性走 proto/valid 规则 */
    DG_CHECK(web_auth_set("ab", "newpass123") == DG_ERR_BAD_UID);   /* 太短 */
    DG_CHECK(web_auth_set("admin;rm -rf", "newpass123") == DG_ERR_BAD_UID);
    DG_CHECK(web_auth_set("guard01", "abc") == DG_ERR_BAD_PWD);     /* 太短 */
    DG_CHECK(web_auth_set("guard01", "has space") == DG_ERR_BAD_PWD);
    DG_CHECK(web_auth_set("guard01", "Str0ng-Pass") == DG_OK);
    DG_CHECK(web_auth_is_default() == false);         /* 已改密:标记清除 */

    char u2[WEB_AUTH_USER_MAX] = "";
    DG_CHECK(web_auth_get_user(u2, sizeof(u2)) == DG_OK);
    DG_CHECK(strcmp(u2, "guard01") == 0);
    DG_CHECK(web_auth_verify("guard01", "Str0ng-Pass") == DG_OK);
    DG_CHECK(web_auth_verify("admin", "admin") == DG_ERR_WRONG_PASSWORD); /* 旧凭据失效 */

    /* 改口令需旧口令 */
    DG_CHECK(web_auth_change_pwd("bad", "Another1") == DG_ERR_WRONG_PASSWORD);
    DG_CHECK(web_auth_change_pwd("Str0ng-Pass", "Another1") == DG_OK);
    DG_CHECK(web_auth_verify("guard01", "Another1") == DG_OK);
}

static void t_sessions(void)
{
    printf("[WA2] 会话表:签发/校验/过期/注销/表满\n");
    web_session_revoke_all();
    time_t now = time(NULL);

    char t1[WEB_TOKEN_LEN + 1] = "";
    int expires = 0;
    DG_CHECK(web_session_create(t1, sizeof(t1), &expires) == DG_OK);
    DG_CHECK(strlen(t1) == WEB_TOKEN_LEN);
    DG_CHECK(expires == WEB_SESSION_TTL_S);
    DG_CHECK(web_session_validate(t1, now) == true);
    DG_CHECK(web_session_count(now) == 1);

    /* 伪造 token / 空 token 一律拒 */
    DG_CHECK(web_session_validate("deadbeef", now) == false);
    DG_CHECK(web_session_validate("", now) == false);
    DG_CHECK(web_session_validate(NULL, now) == false);

    /* 过期:用注入的 now 快进,不用睡 */
    DG_CHECK(web_session_validate(t1, now + WEB_SESSION_TTL_S + 1) == false);
    DG_CHECK(web_session_count(now + WEB_SESSION_TTL_S + 1) == 0);

    /* 注销单条 */
    char t2[WEB_TOKEN_LEN + 1] = "";
    DG_CHECK(web_session_create(t2, sizeof(t2), NULL) == DG_OK);
    DG_CHECK(web_session_validate(t2, now) == true);
    web_session_revoke(t2);
    DG_CHECK(web_session_validate(t2, now) == false);

    /* 表满:第 N+1 次签发仍必须给可用 token(顶掉最旧的) */
    char toks[WEB_SESSION_MAX + 1][WEB_TOKEN_LEN + 1];
    for (int i = 0; i <= WEB_SESSION_MAX; i++) {
        DG_CHECK(web_session_create(toks[i], WEB_TOKEN_LEN + 1, NULL) == DG_OK);
    }
    time_t later = time(NULL);
    DG_CHECK(web_session_validate(toks[WEB_SESSION_MAX], later) == true);
    DG_CHECK(web_session_count(later) == WEB_SESSION_MAX);   /* 不超上限 */

    /* 全吊销(改凭据时调用) */
    web_session_revoke_all();
    DG_CHECK(web_session_count(later) == 0);
}

static void t_cred_change_kills_sessions(void)
{
    printf("[WA3] 改凭据即踢下线(旧 token 失效)\n");
    web_session_revoke_all();
    time_t now = time(NULL);
    char tok[WEB_TOKEN_LEN + 1] = "";
    DG_CHECK(web_session_create(tok, sizeof(tok), NULL) == DG_OK);
    DG_CHECK(web_session_validate(tok, now) == true);

    DG_CHECK(web_auth_set("guard01", "Zx9-Passwd") == DG_OK);
    DG_CHECK(web_session_validate(tok, now) == false);
    DG_CHECK(web_session_count(now) == 0);
}

static void t_login_throttle(void)
{
    printf("[WA4] 登录风控:连错锁定 / 不同来源互不影响 / 成功即解锁\n");
    const char *ip = "192.168.2.50";
    int retry = 0;
    DG_CHECK(web_auth_login_blocked(ip, &retry) == false);

    for (int i = 0; i < 4; i++)
        DG_CHECK(web_auth_login_fail(ip) == false);       /* 未达阈值:不锁 */
    DG_CHECK(web_auth_login_blocked(ip, &retry) == false);

    DG_CHECK(web_auth_login_fail(ip) == true);            /* 第 5 次:锁定 */
    DG_CHECK(web_auth_login_blocked(ip, &retry) == true);
    DG_CHECK(retry > 0 && retry <= 60);

    /* 别的来源不受影响(别把整个局域网一起锁死) */
    DG_CHECK(web_auth_login_blocked("192.168.2.51", NULL) == false);

    /* 成功登录清零计数:解锁后再错一次不该立刻又被锁 */
    web_auth_login_ok(ip);
    DG_CHECK(web_auth_login_blocked(ip, NULL) == false);
    DG_CHECK(web_auth_login_fail(ip) == false);

    /* 锁定来源仍可被其它 IP 的登录请求正常处理(NULL/异常入参不崩) */
    DG_CHECK(web_auth_login_blocked(NULL, NULL) == false);
    (void)web_auth_login_fail(NULL);
}

int main(void)
{
    fresh_storage();
    t_credentials();
    t_sessions();
    t_cred_change_kills_sessions();
    t_login_throttle();
    storage_deinit();
    DG_TEST_EXIT();
}
