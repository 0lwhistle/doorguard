/*
 * test_enroll_ic.c — IC 绑卡链路端到端测试(ICCARD_PROTOCOL §7.3)
 *
 * 链路:EV_ENROLL_REQUEST(IC)→ enroll 置录卡态 + FLUSH → sim 后端注入帧
 *       → card_provider 发 EV_IC_CARD → enroll 查重/落库/写方式位
 *       → EV_ENROLL_RESULT。
 * 覆盖:绑定成功(ic_card+auth_flags)、他人卡 DUP_IC、同卡重绑幂等、
 * 录入态取消+换会话 FLUSH(旧帧不串)、解绑清位。
 */
#include "dg_test.h"
#include "card_provider.h"
#include "cfg.h"
#include "enroll_service.h"
#include "event_bus.h"
#include "events.h"
#include "iccard_hal.h"
#include "storage.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static atomic_int s_result_cnt[16];
static atomic_int s_result_err[16];

static int on_result(const event_t *e, void *ud)
{
    (void)ud;
    const ev_enroll_result_t *r = (const ev_enroll_result_t *)e->data;
    if (r->kind >= 0 && r->kind < 16) {
        atomic_store(&s_result_err[r->kind], r->err);
        atomic_fetch_add(&s_result_cnt[r->kind], 1);
    }
    return 0;
}

static void wait_cnt(int kind, int want, int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 5; i++) {
        if (atomic_load(&s_result_cnt[kind]) >= want)
            return;
        usleep(5000);
    }
}

static void publish_req(const char *uid, int32_t kind, int32_t arg)
{
    ev_enroll_request_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.user_id, sizeof(ev.user_id), "%s", uid);
    ev.kind = kind;
    ev.arg = arg;
    ev.seq = (uint32_t)time(NULL);
    EVENT_BUS_PUBLISH(EV_ENROLL_REQUEST, &ev);
}

static user_rec_t get_user(const char *uid)
{
    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    db_user_get(uid, &rec);
    return rec;
}

static int s_card_seq;
static void inject_hex(const char *hex)
{
    /* hex → uid 字节(测试自用,大小写不敏感) */
    uint8_t uid[DG_ICCARD_UID_MAX];
    size_t n = strlen(hex) / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(hex + i * 2, "%2x", &v);
        uid[i] = (uint8_t)v;
    }
    iccard_sim_inject(uid, (uint8_t)n);
    s_card_seq++;
}

static void make_user(const char *uid, const char *name)
{
    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.user_id, sizeof(rec.user_id), "%s", uid);
    snprintf(rec.user_name, sizeof(rec.user_name), "%s", name);
    rec.role = DG_ROLE_NORMAL;
    rec.auth_flags = DG_AUTH_FACE | DG_AUTH_PWD;
    DG_CHECK(db_user_set_password(&rec, "abcd1234") == DG_OK);
    DG_CHECK(db_user_add(&rec) == DG_OK);
}

int main(void)
{
    DG_CHECK(event_bus_init() == EVENT_BUS_OK);

    char dir[128];
    snprintf(dir, sizeof(dir), "/tmp/dg_enroll_ic_%d", (int)getpid());
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", dir, dir);
    DG_CHECK(system(cmd) == 0);

    /* 读卡器走 sim 后端:出厂模板指向真实节点的宿主默认值改为 "sim" */
    char cfg_path[160];
    snprintf(cfg_path, sizeof(cfg_path), "%s/default.json", dir);
    FILE *f = fopen(cfg_path, "w");
    DG_CHECK(f != NULL);
    fprintf(f, "{\"iccard\":{\"dev_path\":\"sim\"}}\n");
    fclose(f);

    char db[192], key[192], cur[192];
    snprintf(db, sizeof(db), "%s/db.sqlite", dir);
    snprintf(key, sizeof(key), "%s/dg.key", dir);
    snprintf(cur, sizeof(cur), "%s/cur.json", dir);
    DG_CHECK(storage_init(db, key) == DG_OK);
    /* 注意:cfg_load 任一路径为 NULL 即纯内置默认(读不到 default.json),
     * sim 后端注入必须靠出厂模板里的 iccard.dev_path,两路都给 */
    DG_CHECK(cfg_load(cfg_path, cur) == DG_OK);
    DG_CHECK(strcmp(cfg_get()->iccard_dev_path, "sim") == 0);

    DG_CHECK(enroll_service_start() == DG_OK);
    DG_CHECK(card_provider_start() == DG_OK);
    event_bus_subscribe(EV_ENROLL_RESULT, on_result, NULL);

    make_user("20001", "张三");
    make_user("20002", "李四");

    printf("[P1] 绑定成功:ic_card 落库 + 方式位打开\n");
    publish_req("20001", DG_ENROLL_IC, 0);
    usleep(400 * 1000);                   /* 录入态生效 + 旧会话 FLUSH */
    inject_hex("04A3B2C1");
    wait_cnt(DG_ENROLL_IC, 1, 3000);
    DG_CHECK(atomic_load(&s_result_err[DG_ENROLL_IC]) == DG_OK);
    user_rec_t a = get_user("20001");
    DG_CHECK(strcmp(a.ic_card, "04A3B2C1") == 0);
    DG_CHECK(a.auth_flags & DG_AUTH_IC);

    printf("[P2] 他人卡重复:DUP_IC,目标用户不被绑定\n");
    publish_req("20002", DG_ENROLL_IC, 0);
    usleep(100 * 1000);
    inject_hex("04A3B2C1");
    wait_cnt(DG_ENROLL_IC, 2, 3000);
    DG_CHECK(atomic_load(&s_result_err[DG_ENROLL_IC]) == DG_ERR_DUP_IC);
    user_rec_t b = get_user("20002");
    DG_CHECK(b.ic_card[0] == '\0');
    DG_CHECK(!(b.auth_flags & DG_AUTH_IC));

    printf("[P3] 同卡重绑自身:幂等成功\n");
    publish_req("20001", DG_ENROLL_IC, 0);
    usleep(100 * 1000);
    inject_hex("04A3B2C1");
    wait_cnt(DG_ENROLL_IC, 3, 3000);
    DG_CHECK(atomic_load(&s_result_err[DG_ENROLL_IC]) == DG_OK);

    printf("[P4] 换会话 FLUSH:录入态切换前留在管道里的旧帧不串绑\n");
    inject_hex("0A0B0C0D");               /* 无人在录入态:帧滞留管道 */
    usleep(100 * 1000);
    publish_req("20002", DG_ENROLL_IC, 0);/* 进录入态 → FLUSH 清掉旧帧 */
    usleep(500 * 1000);                   /* provider 200ms 粒度执行 flush */
    inject_hex("11223344");               /* 新卡才是本会话的 */
    wait_cnt(DG_ENROLL_IC, 4, 3000);
    DG_CHECK(atomic_load(&s_result_err[DG_ENROLL_IC]) == DG_OK);
    b = get_user("20002");
    DG_CHECK(strcmp(b.ic_card, "11223344") == 0);   /* 不是 0A0B0C0D */

    printf("[P5] 取消录入态:注入的卡不被绑定\n");
    publish_req("20002", DG_ENROLL_IC, 0);
    usleep(100 * 1000);
    publish_req("20002", DG_ENROLL_IC_CANCEL, 0);
    usleep(500 * 1000);                   /* FLUSH 生效后再注入 */
    int cnt_before = atomic_load(&s_result_cnt[DG_ENROLL_IC]);
    inject_hex("55667788");
    usleep(300 * 1000);
    DG_CHECK(atomic_load(&s_result_cnt[DG_ENROLL_IC]) == cnt_before);
    b = get_user("20002");
    DG_CHECK(strcmp(b.ic_card, "11223344") == 0);

    printf("[P6] 解绑:卡号清空 + 方式位回收\n");
    publish_req("20001", DG_ENROLL_IC_CLEAR, 0);
    wait_cnt(DG_ENROLL_IC_CLEAR, 1, 3000);
    DG_CHECK(atomic_load(&s_result_err[DG_ENROLL_IC_CLEAR]) == DG_OK);
    a = get_user("20001");
    DG_CHECK(a.ic_card[0] == '\0');
    DG_CHECK(!(a.auth_flags & DG_AUTH_IC));
    DG_CHECK(a.auth_flags & DG_AUTH_FACE);          /* 别误清其他位 */

    card_provider_stop();
    enroll_service_stop();
    storage_deinit();
    DG_TEST_EXIT();
}
