/*
 * test_enroll_flow.c — 录入链路端到端测试(宿主;视觉用 sim mock 后端)
 *
 * 链路:EV_ENROLL_REQUEST → enroll_service → EV_VISION_CAPTURE_REQ
 *       → sim 后端提交伪特征 → EV_VISION_FEATURE → **草稿槽(不落库)**
 *       → EV_ENROLL_RESULT(OK = 草稿就绪);commit_draft 才落库。
 *
 * 背景(2026-09-21):板上"录入无响应"暴露这条链从未被端到端测过——单元各自
 * 为真,拼起来没人验。本测试把链钉死:请求→回执→DB 状态一致。
 *
 * 2026-09-27 两段式草稿改版:采集回执 OK 后 DB 必须**未变**(草稿在
 * enroll 服务内),UI「保存」= commit_draft 落库、「放弃」= discard_draft
 * 擦除;清除走同步 clear_face;删除用户连带丢草稿。
 *
 * 覆盖:采集→草稿(DB 不变)/ commit 落库(特征+头像)/ 重采→discard /
 * clear_face 同步清除 / 对不存在用户采集(当场失败)/ 删除连带丢草稿 /
 * 无草稿 commit 拒绝 / user_save 建用户与覆写语义(A1 UI「保存」收口)。
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

/* 方式位读回(用户不存在返回 0xFFFFFFFF 哨兵) */
static uint32_t user_flags(const char *uid)
{
    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    if (db_user_get(uid, &rec) != DG_OK)
        return 0xFFFFFFFFu;
    return rec.auth_flags;
}

/* 头像落库形态:返回 JPEG 字节数;0 = 未录头像;-1 = 用户不存在/形态异常 */
static int user_avatar_len(const char *uid)
{
    static uint8_t jpeg[DG_AVATAR_JPEG_MAX];
    size_t len = 0;
    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    if (db_user_get(uid, &rec) != DG_OK)
        return -1;                      /* 用户不存在 */
    if (db_user_get_avatar(uid, jpeg, sizeof(jpeg), &len) == DG_ERR_NOT_FOUND)
        return 0;                       /* 有用户没头像 */
    if (len < 2 || jpeg[0] != 0xFF || jpeg[1] != 0xD8)
        return -1;                      /* 不是 JPEG:加密链路或编码出错 */
    return (int)len;
}

/* 草稿头像形态:true = JPEG 明文可取(SOI 头可见) */
static bool draft_avatar_ok(const char *uid)
{
    const uint8_t *jpeg = NULL;
    size_t len = 0;
    if (!enroll_service_draft_avatar(uid, &jpeg, &len))
        return false;
    return len >= 2 && jpeg[0] == 0xFF && jpeg[1] == 0xD8;
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

static char s_dir[64];

/* ④b 注入桩:模拟后端降级(add 失败)与恢复(恒成功) */
static int fail_add(const char *uid, const uint8_t *f, uint16_t n)
{ (void)uid; (void)f; (void)n; return DG_ERR_NO_MEMORY; }
static int ok_add(const char *uid, const uint8_t *f, uint16_t n)
{ (void)uid; (void)f; (void)n; return DG_OK; }
static int ok_del(const char *uid)
{ (void)uid; return DG_OK; }

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
    char cmd[320];
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

    /* ---- 建用户(编辑页"保存"等价操作:密码必设) ----
     * 方式位不变式(2026-10-04)下建号只有密码位;人脸位由 commit 置位 */
    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.user_id, sizeof(rec.user_id), "10001");
    snprintf(rec.user_name, sizeof(rec.user_name), "张三");
    rec.role = DG_ROLE_NORMAL;
    rec.auth_flags = DG_AUTH_PWD;
    DG_CHECK(db_user_set_password(&rec, "abcd1234") == DG_OK);
    DG_CHECK(db_user_add(&rec) == DG_OK);
    DG_CHECK(user_face_len("10001") == 0);      /* 初始无人脸 */

    /* ---- ① 采集:回执 OK 但 DB 未变,草稿在服务内(两段式核心) ---- */
    publish_req("10001", DG_ENROLL_FACE);
    wait_cnt(&s_result_cnt[DG_ENROLL_FACE], 1, 3000);
    DG_CHECK(atomic_load(&s_result_cnt[DG_ENROLL_FACE]) == 1);
    DG_CHECK(atomic_load(&s_result_err[DG_ENROLL_FACE]) == DG_OK);
    DG_CHECK(user_face_len("10001") == 0);      /* 未落库 */
    DG_CHECK(user_avatar_len("10001") == 0);    /* 头像也未落库 */
    DG_CHECK(enroll_service_draft_active("10001"));
    DG_CHECK(draft_avatar_ok("10001"));         /* 草稿头像 = JPEG 明文 */

    /* ---- ② commit = UI「保存」:特征+头像一次落库,草稿消费;
     * 录入写位:人脸位自动开启(与指纹/IC 同语义,2026-10-04) ---- */
    DG_CHECK(enroll_service_commit_draft("10001") == DG_OK);
    const int len1 = user_face_len("10001");
    DG_CHECK(len1 > 0);
    DG_CHECK(len1 <= DG_FEATURE_MAX);
    const int av1 = user_avatar_len("10001");
    DG_CHECK(av1 > 0);
    printf("  avatar jpeg %d B\n", av1);
    DG_CHECK(!enroll_service_draft_active("10001"));
    DG_CHECK((user_flags("10001") & DG_AUTH_FACE) != 0);   /* 录入写位 */

    /* ---- ③ 重采 → discard:UI「直接退出」放弃,DB 保持原值 ---- */
    publish_req("10001", DG_ENROLL_FACE);
    wait_cnt(&s_result_cnt[DG_ENROLL_FACE], 2, 3000);
    DG_CHECK(atomic_load(&s_result_err[DG_ENROLL_FACE]) == DG_OK);
    DG_CHECK(enroll_service_draft_active("10001"));
    enroll_service_discard_draft("10001");
    DG_CHECK(!enroll_service_draft_active("10001"));
    DG_CHECK(user_face_len("10001") == len1);   /* 旧值未被动过 */
    DG_CHECK(user_avatar_len("10001") == av1);

    /* ---- ④ 重采 → commit → clear_face 同步清除(保留用户) ---- */
    publish_req("10001", DG_ENROLL_FACE);
    wait_cnt(&s_result_cnt[DG_ENROLL_FACE], 3, 3000);
    DG_CHECK(enroll_service_commit_draft("10001") == DG_OK);
    DG_CHECK(user_face_len("10001") == len1);
    DG_CHECK(user_avatar_len("10001") > 0);
    DG_CHECK(enroll_service_clear_face("10001") == DG_OK);
    DG_CHECK(user_face_len("10001") == 0);
    DG_CHECK(user_avatar_len("10001") == 0);    /* 清人脸连带头像消失 */
    DG_CHECK((user_flags("10001") & DG_AUTH_FACE) == 0);  /* 清除清位 */

    /* ---- ④b commit 时内存特征库写失败:DB 必须还原,不留半状态(2026-09-28) ----
     * 注入点 = vision_service_set_lib_ops(装配层同款接线):后端降级
     * (!ready)/库满会让 library_add 失败。期望:commit 返回错误、DB 人脸
     * 保持清除态(无人脸时)或旧值、草稿保留可重试;恢复后重提成功 */
    publish_req("10001", DG_ENROLL_FACE);
    wait_cnt(&s_result_cnt[DG_ENROLL_FACE], 4, 3000);
    DG_CHECK(atomic_load(&s_result_err[DG_ENROLL_FACE]) == DG_OK);
    DG_CHECK(enroll_service_draft_active("10001"));
    vision_service_set_lib_ops(fail_add, ok_del);
    DG_CHECK(enroll_service_commit_draft("10001") != DG_OK);
    DG_CHECK(user_face_len("10001") == 0);      /* DB 被还原,不是半状态 */
    DG_CHECK(enroll_service_draft_active("10001"));  /* 草稿保留可重试 */
    vision_service_set_lib_ops(NULL, NULL);     /* 恢复:后端原 ops 由下次 start 重接 */
    DG_CHECK(enroll_service_commit_draft("10001") != DG_OK); /* NULL ops 仍失败,防误成功 */
    vision_service_set_lib_ops(ok_add, ok_del); /* 给个恒成功 add 恢复链路 */
    DG_CHECK(enroll_service_commit_draft("10001") == DG_OK);
    DG_CHECK(user_face_len("10001") > 0);

    /* ---- ⑤ 对不存在用户采集:回执当场失败,且不产生草稿 ---- */
    publish_req("99999", DG_ENROLL_FACE);
    wait_cnt(&s_result_cnt[DG_ENROLL_FACE], 5, 3000);
    DG_CHECK(atomic_load(&s_result_err[DG_ENROLL_FACE]) != DG_OK);
    DG_CHECK(user_face_len("99999") == -1);     /* 用户依旧不存在 */
    DG_CHECK(!enroll_service_draft_active("99999"));

    /* ---- ⑥ 无草稿 commit 拒绝(防误提交) ---- */
    DG_CHECK(enroll_service_commit_draft("10001") == DG_ERR_NOT_FOUND);

    /* ---- ⑦ 删除用户连带丢草稿(DB+特征库一致,草稿不残留) ---- */
    publish_req("10001", DG_ENROLL_FACE);
    wait_cnt(&s_result_cnt[DG_ENROLL_FACE], 6, 3000);
    DG_CHECK(enroll_service_draft_active("10001"));
    publish_req("10001", DG_ENROLL_DELETE);
    wait_cnt(&s_result_cnt[DG_ENROLL_DELETE], 1, 3000);
    DG_CHECK(atomic_load(&s_result_err[DG_ENROLL_DELETE]) == DG_OK);
    DG_CHECK(user_face_len("10001") == -1);
    DG_CHECK(user_avatar_len("10001") == -1);   /* 用户即删,头像随之消失 */
    DG_CHECK(!enroll_service_draft_active("10001"));

    /* ---- ⑧ user_save 语义(A1:编辑页「保存」收口进 enroll 服务)----
     * ADD 建用户(密码必设/auth_flags=密码位)/ EDIT 覆写字段(密码
     * NULL=保持、ic_card 等未动字段不解绑)/ 错误码透传 / user_page 分页读 */
    DG_CHECK(enroll_service_user_save("20001", "李四", DG_ROLE_NORMAL, NULL)
             == DG_ERR_NO_PASSWORD);                 /* 无密码禁止建用户 */
    DG_CHECK(enroll_service_user_get("20001", &rec) == DG_ERR_NOT_FOUND);
    DG_CHECK(enroll_service_user_save("20001", "李四", DG_ROLE_NORMAL,
                                      "abcd1234") == DG_OK);
    DG_CHECK(enroll_service_user_get("20001", &rec) == DG_OK);
    DG_CHECK(rec.auth_flags == DG_AUTH_PWD);   /* 不变式:建号只有密码位 */
    DG_CHECK(db_verify_password("20001", "abcd1234", &rec) == DG_OK);

    /* EDIT:覆写姓名/权限;密码 NULL = 保持;ic_card 以库内记录为基线不解绑 */
    snprintf(rec.ic_card, sizeof(rec.ic_card), "CARD-20001");
    DG_CHECK(db_user_update(&rec) == DG_OK);         /* 造绑卡基线(仅测内) */
    DG_CHECK(enroll_service_user_save("20001", "李四丰", DG_ROLE_ADMIN, NULL)
             == DG_OK);
    DG_CHECK(enroll_service_user_get("20001", &rec) == DG_OK);
    DG_CHECK(rec.role == DG_ROLE_ADMIN);
    DG_CHECK(strcmp(rec.user_name, "李四丰") == 0);
    DG_CHECK(strcmp(rec.ic_card, "CARD-20001") == 0); /* 零基线覆写会解绑卡 */
    DG_CHECK(db_verify_password("20001", "abcd1234", &rec) == DG_OK);
    DG_CHECK(enroll_service_user_save("20001", "李四丰", DG_ROLE_ADMIN,
                                      "xyz98765") == DG_OK);          /* EDIT 改密 */
    DG_CHECK(db_verify_password("20001", "xyz98765", &rec) == DG_OK);
    DG_CHECK(db_verify_password("20001", "abcd1234", &rec)
             == DG_ERR_WRONG_PASSWORD);
    /* 错误码透传:已存在 uid = EDIT 覆写语义(不报重号);空姓名走
     * db_user_add 权威校验,storage 码原样冒出 */
    DG_CHECK(enroll_service_user_save("29999", "", DG_ROLE_NORMAL, "pass1234")
             == DG_ERR_PARAM);
    DG_CHECK(enroll_service_user_get("29999", &rec) == DG_ERR_NOT_FOUND);

    DG_CHECK(enroll_service_user_save("20002", "王五", DG_ROLE_NORMAL,
                                      "pass1234") == DG_OK);
    DG_CHECK(enroll_service_user_save("20003", "赵六", DG_ROLE_BLACKLIST,
                                      "pass1234") == DG_OK);
    {
        enroll_user_row_t rows[2];
        uint32_t n = 0, total = 0;
        DG_CHECK(enroll_service_user_page(rows, 2, &n, &total) == DG_OK);
        DG_CHECK(n == 2 && total == 3);              /* 字典序首页 + 全量计数 */
        DG_CHECK(strcmp(rows[0].user_id, "20001") == 0);
        DG_CHECK(strcmp(rows[1].user_id, "20002") == 0);
        DG_CHECK(strcmp(rows[0].user_name, "李四丰") == 0);
        DG_CHECK(rows[0].role == DG_ROLE_ADMIN);
        DG_CHECK(enroll_service_user_page(rows, ENROLL_USER_PAGE_MAX + 1,
                                          &n, &total) == DG_ERR_PARAM);
    }

    /* ---- ⑨ web 静态图上传(sim 后端 mock 提取;2026-10-04)----
     * 链路:face_upload 受理(单飞+槽)→ EV_VISION_STILL_REQ → sim 提交
     * 伪特征/头像 → EV_VISION_FEATURE → 直落库(不经草稿)→ 回执。
     * 与草稿路径同尾:查重/置位/头像,结果按 seq 回执 */
    {
        static uint8_t jpg[4096];
        memcpy(jpg, "\xFF\xD8\xFF\xE0", 4);      /* SOI 形态(sim 不解析内容) */
        memset(jpg + 4, 0xA5, sizeof(jpg) - 4);
        uint32_t seq = 0;

        DG_CHECK(enroll_service_upload_busy() == false);
        DG_CHECK(enroll_service_face_upload("20002", jpg, sizeof(jpg), &seq)
                 == DG_OK);
        DG_CHECK(seq != 0);
        DG_CHECK(enroll_service_upload_busy() == true);   /* 受理即单飞闸 */
        wait_cnt(&s_result_cnt[DG_ENROLL_FACE], 7, 3000);
        DG_CHECK(atomic_load(&s_result_err[DG_ENROLL_FACE]) == DG_OK);
        DG_CHECK(enroll_service_upload_busy() == false);  /* 回执即解除 */
        DG_CHECK(user_face_len("20002") > 0);             /* 直落库 */
        DG_CHECK(user_avatar_len("20002") > 0);
        DG_CHECK((user_flags("20002") & DG_AUTH_FACE) != 0);  /* 录入写位 */

        /* 重传 = 覆盖(重录语义);迟到槽位/串号由 seq 配对挡 */
        DG_CHECK(enroll_service_face_upload("20002", jpg, 128, &seq) == DG_OK);
        wait_cnt(&s_result_cnt[DG_ENROLL_FACE], 8, 3000);
        DG_CHECK(atomic_load(&s_result_err[DG_ENROLL_FACE]) == DG_OK);
        DG_CHECK(user_face_len("20002") > 0);

        /* 边界:不存在用户 / 超上限 / 空体,全部当场拒,不进提取链路 */
        DG_CHECK(enroll_service_face_upload("99999", jpg, 128, &seq)
                 == DG_ERR_NOT_FOUND);
        {
            static uint8_t big[DG_FACE_UPLOAD_MAX + 1];
            DG_CHECK(enroll_service_face_upload("20002", big, sizeof(big), &seq)
                     == DG_ERR_PARAM);
        }
        DG_CHECK(enroll_service_face_upload("20002", jpg, 0, &seq)
                 == DG_ERR_PARAM);
        DG_CHECK(enroll_service_upload_busy() == false);
    }

    cleanup();
    DG_TEST_EXIT();
}
