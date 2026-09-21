/*
 * test_enroll_flow.c — 录入链路端到端测试(宿主;视觉用 sim mock 后端)
 *
 * 链路:EV_ENROLL_REQUEST → enroll_service → EV_VISION_CAPTURE_REQ
 *       → sim 后端提交伪特征 → EV_VISION_FEATURE → enroll 查重入库
 *       → EV_ENROLL_RESULT 回执。
 *
 * 背景(2026-09-21):板上"录入无响应"暴露这条链从未被端到端测过——单元各自
 * 为真,拼起来没人验。本测试把链钉死:请求→回执→DB 状态一致。
 *
 * 覆盖:录入(建用户后)/ 重录 / 清除人脸 / 对不存在用户录入(失败回执)/
 *       删除用户(DB + 特征库一致)。
 */
#include "dg_test.h"
#include "event_bus.h"
#include "events.h"
#include "cfg.h"
#include "storage.h"
#include "vision_service.h"
#include "vision_backend.h"
#include "enroll_service.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static atomic_int s_result_cnt[4];          /* 按 dg_enroll_kind_t 计数 */
static atomic_int s_result_err[4];          /* 最近一次该 kind 的 err */

static int on_result(const event_t *e, void *ud)
{
    (void)ud;
    const ev_enroll_result_t *r = (const ev_enroll_result_t *)e->data;
    if (r->kind >= 0 && r->kind < 4) {
        atomic_store(&s_result_err[r->kind], r->err);
        atomic_fetch_add(&s_result_cnt[r->kind], 1);
    }
    return 0;
}

static void wait_cnt(volatile atomic_int *c, int want, int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 5; i++) {
        if (atomic_load(c) >= want)
            return;
        usleep(5000);
    }
}

static int user_face_len(const char *uid)
{
    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    if (db_user_get(uid, &rec) != DG_OK)
        return -1;
    return rec.face_vec_len;
}

static void publish_req(const char *uid, int32_t kind)
{
    ev_enroll_request_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.user_id, sizeof(ev.user_id), "%s", uid);
    ev.kind = kind;
    ev.seq = (uint32_t)time(NULL) * 10 + kind;   /* 进程内唯一即可 */
    EVENT_BUS_PUBLISH(EV_ENROLL_REQUEST, &ev);
}

static char s_dir[96];

static void cleanup(void)
{
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", s_dir);
    if (system(cmd) != 0)
        printf("  清理沙箱失败(忽略)\n");
}

int main(void)
{
    DG_CHECK(event_bus_init() == EVENT_BUS_OK);
    snprintf(s_dir, sizeof(s_dir), "/tmp/dg_enroll_%d", (int)getpid());
    char cmd[192];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", s_dir, s_dir);
    DG_CHECK(system(cmd) == 0);
    char db[192], key[192];
    snprintf(db, sizeof(db), "%s/db.sqlite", s_dir);
    snprintf(key, sizeof(key), "%s/dg.key", s_dir);
    DG_CHECK(storage_init(db, key) == DG_OK);
    cfg_load(NULL, NULL);                       /* 无文件模式,纯默认 */

    DG_CHECK(enroll_service_start() == DG_OK);
    DG_CHECK(vision_service_start() == DG_OK);
    /* 装配层(main.c)在测试里缺席:显式注册 sim 后端并启动,
     * 让 CAPTURE_REQ 得到应答(mock 伪特征)——这正是板上 rknn/rockiva 后端的位置 */
    extern const vision_backend_ops_t vision_backend_sim;
    DG_CHECK(vision_backend_register(&vision_backend_sim) == DG_OK);
    DG_CHECK(vision_backend_start(true) == DG_OK);

    event_bus_subscribe(EV_ENROLL_RESULT, on_result, NULL);

    /* ---- 建用户(编辑页"保存"等价操作:密码必设) ---- */
    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.user_id, sizeof(rec.user_id), "10001");
    snprintf(rec.user_name, sizeof(rec.user_name), "张三");
    rec.role = DG_ROLE_NORMAL;
    rec.auth_flags = DG_AUTH_FACE | DG_AUTH_PWD;
    DG_CHECK(db_user_set_password(&rec, "abcd1234") == DG_OK);
    DG_CHECK(db_user_add(&rec) == DG_OK);
    {
        user_rec_t tmp; memset(&tmp,0,sizeof(tmp));
        int rc = db_user_get("10001", &tmp);
        printf("  debug: rc=%d face_len=%u name='%s'\n", rc, tmp.face_vec_len, tmp.user_name);
        printf("  debug2: user_face_len=%d\n", user_face_len("10001"));
    }
    DG_CHECK(user_face_len("10001") == 0);      /* 初始无人脸 */

    /* ---- ① 录入人脸:请求 → 回执 OK → DB 有特征 ---- */
    publish_req("10001", DG_ENROLL_FACE);
    wait_cnt(&s_result_cnt[DG_ENROLL_FACE], 1, 3000);
    DG_CHECK(atomic_load(&s_result_cnt[DG_ENROLL_FACE]) == 1);
    DG_CHECK(atomic_load(&s_result_err[DG_ENROLL_FACE]) == DG_OK);
    const int len1 = user_face_len("10001");
    DG_CHECK(len1 > 0);
    DG_CHECK(len1 <= DG_FEATURE_MAX);

    /* ---- ② 重录:长度刷新(伪特征同人恒定,长度不变但流程要通) ---- */
    publish_req("10001", DG_ENROLL_FACE);
    wait_cnt(&s_result_cnt[DG_ENROLL_FACE], 2, 3000);
    DG_CHECK(atomic_load(&s_result_err[DG_ENROLL_FACE]) == DG_OK);
    DG_CHECK(user_face_len("10001") == len1);

    /* ---- ③ 清除人脸(保留用户) ---- */
    publish_req("10001", DG_ENROLL_FACE_CLEAR);
    wait_cnt(&s_result_cnt[DG_ENROLL_FACE_CLEAR], 1, 3000);
    DG_CHECK(atomic_load(&s_result_err[DG_ENROLL_FACE_CLEAR]) == DG_OK);
    DG_CHECK(user_face_len("10001") == 0);

    /* ---- ④ 对不存在用户录入:回执失败,而不是无声 ---- */
    publish_req("99999", DG_ENROLL_FACE);
    wait_cnt(&s_result_cnt[DG_ENROLL_FACE], 3, 3000);
    DG_CHECK(atomic_load(&s_result_err[DG_ENROLL_FACE]) != DG_OK);
    DG_CHECK(user_face_len("99999") == -1);     /* 用户依旧不存在 */

    /* ---- ⑤ 再录入 → 删除用户:DB 没了,特征库同步(经同一事件) ---- */
    publish_req("10001", DG_ENROLL_FACE);
    wait_cnt(&s_result_cnt[DG_ENROLL_FACE], 4, 3000);
    DG_CHECK(user_face_len("10001") == len1);
    publish_req("10001", DG_ENROLL_DELETE);
    wait_cnt(&s_result_cnt[DG_ENROLL_DELETE], 1, 3000);
    DG_CHECK(atomic_load(&s_result_err[DG_ENROLL_DELETE]) == DG_OK);
    DG_CHECK(user_face_len("10001") == -1);

    cleanup();
    DG_TEST_EXIT();
}
