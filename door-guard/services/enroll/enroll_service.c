/*
 * enroll_service.c — 录入编排实现
 *
 * 2026-09-27 两段式草稿:特征/头像采集后**暂存服务内草稿槽,不落库**——
 * 用户在编辑页「保存」才 commit(查重+DB+特征库+头像),退出可 discard。
 * 编辑页对「姓名/权限/密码」本来就是攒草稿保存,特征此前却是采集即生效,
 * 用户反馈行为不一致(「编辑完直接退出也没有提示」),特此统一。
 *
 * 2026-10-04 两条录入路径归一:设备拍摄走草稿(UI 保存才落库),web 上传
 * 直落(face_commit 公共尾,受理制单飞)。方式位不变式上线——落库成功
 * 自动开启人脸位、清除自动关闭(与指纹/IC 的既有语义对齐)。
 *
 * 查重(spec-database §2:相似度语义)由 storage 的特征比较器执行,在
 * face_commit 时触发——查重冲突从「拍摄时报」后移到「保存时报」,冲突时
 * 草稿保留,用户可重拍覆盖或返回放弃。1:N 查重的真算法比较器在 vision
 * 后端就绪后经 storage_set_feature_cmp 注入。
 */
#include "enroll_service.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"
#include "iccard_hal.h"
#include "storage.h"
#include "vision_service.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "[ENROLL]";

static event_subscription_t *s_subs[4];
static int s_sub_cnt = 0;

/* ---- IC 录入态(enroll 独有;ICCARD_PROTOCOL §7.3) ----
 * 编辑页发起 → 置态 + FLUSH;下一张 EV_IC_CARD 即绑定目标;页面退出取消。
 * 单飞无并发(UI 只有一个编辑页)。 */
typedef struct {
    bool     active;
    char     uid[DG_UID_LEN];
    uint32_t seq;
} ic_enroll_t;

static ic_enroll_t s_ic;

static void ic_state_clear(void)
{
    s_ic.active = false;
    s_ic.uid[0] = '\0';
}

static void ic_flush_req(void)
{
    ev_iccard_ctrl_t ev = { .op = DG_ICCARD_CTRL_FLUSH };
    EVENT_BUS_PUBLISH(EV_ICCARD_CTRL, &ev);
}

/* ---- 人脸草稿槽(单槽,单用户) ----
 * 总线线程写(on_feature),UI 线程读写(commit/discard/预览);互斥锁
 * 保护指针一致性。预览返回内部缓冲指针,依赖「UI 单线程 + 采集与提交
 * 不同时在飞」的使用约定(见头文件),锁只在访问瞬间持有。 */
typedef struct {
    char     uid[DG_UID_LEN];            /* 空 = 无草稿 */
    uint8_t  feature[DG_FEATURE_MAX];
    size_t   feature_len;
    uint8_t  jpeg[DG_AVATAR_JPEG_MAX];
    size_t   jpeg_len;
} face_draft_t;

static face_draft_t s_draft;
static pthread_mutex_t s_draft_mtx = PTHREAD_MUTEX_INITIALIZER;

static void draft_reset(face_draft_t *d)
{
    memset(d->feature, 0, sizeof(d->feature));   /* 明文用后擦除 */
    memset(d->jpeg, 0, sizeof(d->jpeg));
    d->feature_len = 0;
    d->jpeg_len = 0;
    d->uid[0] = '\0';
}

bool enroll_service_draft_active(const char *user_id)
{
    if (!user_id || !user_id[0])
        return false;
    pthread_mutex_lock(&s_draft_mtx);
    const bool active = s_draft.uid[0] && strcmp(s_draft.uid, user_id) == 0;
    pthread_mutex_unlock(&s_draft_mtx);
    return active;
}

bool enroll_service_draft_avatar(const char *user_id, const uint8_t **jpeg,
                                 size_t *len)
{
    if (!user_id || !jpeg || !len || !user_id[0])
        return false;
    pthread_mutex_lock(&s_draft_mtx);
    if (!s_draft.uid[0] || strcmp(s_draft.uid, user_id) != 0 || !s_draft.jpeg_len) {
        pthread_mutex_unlock(&s_draft_mtx);
        return false;
    }
    *jpeg = s_draft.jpeg;
    *len = s_draft.jpeg_len;
    pthread_mutex_unlock(&s_draft_mtx);
    return true;
}

/* 特征落库公共尾:查重(db_user_update 内比较器)→ DB → 内存特征库 → 头像。
 * commit_draft(设备拍摄草稿)与 face_upload(web 上传直落)共用,保证两条
 * 录入路径的落库语义(含方式位置位)永远一致。
 *
 * 事务序 = DB 先行、内存特征库随后,失败反向还原:
 * - DB 失败(查重冲突等):两侧都未动,干净返回;
 * - library_add 失败(后端降级 !ready/库满):按 existing 还原 DB——否则留下
 *   「DB 有新特征、内存没有」的半状态,用户存在却识别不出,直到重启才自愈
 *   (原实现静默忽略返回值的真后果)。还原窗口内内存仍是旧特征、DB 已是
 *   新特征,但两者同属本 uid,不构成越权。
 *
 * 方式位(spec-database §1「录入写位」,2026-10-04 起人脸与指纹/IC 对齐):
 * 成功落库 = 自动开启人脸位(重录被特意关闭的方式会重新可用,关闭需在
 * 「验证方式」里显式操作——与指纹/IC 路径同语义)。ic_card 必须回填:
 * db_user_update 对 ic_card 是"始终覆盖(空=解绑)"语义,不回填会把已绑
 * 卡悄悄解绑(存量 bug,2026-10-04 修)。
 *
 * @param jpeg/jlen 头像 JPEG(可为 NULL/0 = 本次无头像)
 * @return DG_OK;失败码(DUP_FACE/DB 等)原样返回,调用方决定重试或放弃 */
static int face_commit(const char *user_id, const uint8_t *feature, size_t flen,
                       const uint8_t *jpeg, size_t jlen)
{
    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.user_id, sizeof(rec.user_id), "%s", user_id);
    memcpy(rec.face_vec, feature, flen);
    rec.face_vec_len = (uint16_t)flen;

    /* update 为覆盖语义:先取现有记录回填 role/auth_flags/ic_card,避免清零
     * (db_user_update 契约见 storage.h);查重比较器在 update 内触发。
     * existing 同时是回滚依据:内存特征库写失败时把 DB 还原成库内旧值 */
    user_rec_t existing;
    bool had_existing = db_user_get(user_id, &existing) == DG_OK;
    if (had_existing) {
        rec.role = existing.role;
        rec.auth_flags = existing.auth_flags | DG_AUTH_FACE;   /* 录入写位 */
        snprintf(rec.ic_card, sizeof(rec.ic_card), "%s", existing.ic_card);
    } else {
        rec.auth_flags = DG_AUTH_FACE;
    }

    int rc = db_user_update(&rec);
    if (rc != DG_OK) {
        DG_LOGW(TAG, "commit: %s 落库失败(%d)", user_id, rc);
        return rc;
    }

    rc = vision_service_library_add(user_id, feature, flen);
    if (rc != DG_OK) {
        DG_LOGE(TAG, "commit: %s 内存特征库写入失败(%d),还原 DB", user_id, rc);
        if (had_existing && existing.face_vec_len > 0) {
            user_rec_t back = rec;
            memcpy(back.face_vec, existing.face_vec, existing.face_vec_len);
            back.face_vec_len = existing.face_vec_len;
            const int brc = db_user_update(&back);
            if (brc != DG_OK)
                DG_LOGE(TAG, "commit: %s DB 旧特征还原失败(%d),重启后以内存库为准",
                        user_id, brc);
        } else {
            db_user_clear_face(user_id);
        }
        return rc;
    }

    if (jlen && jpeg) {
        const int arc = db_user_set_avatar(user_id, jpeg, jlen);
        if (arc != DG_OK)
            DG_LOGW(TAG, "头像落库失败(%d):特征已入库,头像留空", arc);
    }
    DG_LOGI(TAG, "commit: %s 落库(face %zuB avatar %zuB)", user_id, flen, jlen);
    return DG_OK;
}

int enroll_service_commit_draft(const char *user_id)
{
    if (!user_id || !user_id[0])
        return DG_ERR_PARAM;

    static uint8_t feature[DG_FEATURE_MAX];     /* 出锁后使用,静态不占栈 */
    static uint8_t jpeg[DG_AVATAR_JPEG_MAX];
    size_t flen = 0, jlen = 0;

    pthread_mutex_lock(&s_draft_mtx);
    if (!s_draft.uid[0] || strcmp(s_draft.uid, user_id) != 0) {
        pthread_mutex_unlock(&s_draft_mtx);
        DG_LOGW(TAG, "commit: %s 无草稿", user_id);
        return DG_ERR_NOT_FOUND;
    }
    memcpy(feature, s_draft.feature, s_draft.feature_len);
    flen = s_draft.feature_len;
    if (s_draft.jpeg_len) {
        memcpy(jpeg, s_draft.jpeg, s_draft.jpeg_len);
        jlen = s_draft.jpeg_len;
    }
    pthread_mutex_unlock(&s_draft_mtx);

    const int rc = face_commit(user_id, feature, flen, jlen ? jpeg : NULL, jlen);
    if (rc != DG_OK)
        return rc;              /* 冲突(DUP_FACE 等):草稿保留可重拍或放弃 */

    memset(feature, 0, sizeof(feature));
    memset(jpeg, 0, sizeof(jpeg));
    pthread_mutex_lock(&s_draft_mtx);
    draft_reset(&s_draft);
    pthread_mutex_unlock(&s_draft_mtx);
    return DG_OK;
}

void enroll_service_discard_draft(const char *user_id)
{
    pthread_mutex_lock(&s_draft_mtx);
    if (s_draft.uid[0] && (!user_id || !user_id[0] || strcmp(s_draft.uid, user_id) == 0)) {
        draft_reset(&s_draft);
        DG_LOGI(TAG, "discard: %s 草稿放弃", user_id ? user_id : "*");
    }
    pthread_mutex_unlock(&s_draft_mtx);
}

int enroll_service_clear_face(const char *user_id)
{
    if (!user_id || !user_id[0])
        return DG_ERR_PARAM;
    /* 清除人脸(保留用户):专用接口——db_user_update 的 len=0=保留语义
     * 表达不了清除(2026-09-21 测试抓出);特征库经事件同步删除 */
    int rc = db_user_clear_face(user_id);
    if (rc != DG_OK)
        return rc;
    rc = vision_service_library_remove(user_id);
    /* NOT_INIT = 后端未起(内存库本就为空),视同已清;其余失败留痕:
     * rknn lib_del 契约恒 OK,走到这分支即异常(重启后以 DB 为准自愈) */
    if (rc != DG_OK && rc != DG_ERR_NOT_INIT)
        DG_LOGE(TAG, "clear_face: %s 内存特征库删除异常(%d)", user_id, rc);
    return DG_OK;
}

int enroll_service_finger_pages(const char *user_id, int32_t *pages,
                                uint32_t cap, uint32_t *out_n)
{
    if (!user_id || !user_id[0] || !pages || !out_n)
        return DG_ERR_PARAM;
    return db_finger_list_user(user_id, pages, cap, out_n);
}

/* ---- 用户生命周期读/写(A1 收口;登记见 enroll_service.h 与 proposal §1)---- */

int enroll_service_user_get(const char *user_id, user_rec_t *out)
{
    return db_user_get(user_id, out);
}

int enroll_service_user_page(enroll_user_row_t *rows, uint32_t cap,
                             uint32_t *out_n, uint32_t *out_total)
{
    if (!rows || !out_n || !out_total || !cap || cap > ENROLL_USER_PAGE_MAX)
        return DG_ERR_PARAM;
    char ids[ENROLL_USER_PAGE_MAX][DG_UID_LEN];
    uint32_t n = 0;
    int rc = db_user_list_ids(ids, cap, &n);
    if (rc != DG_OK)
        return rc;
    for (uint32_t i = 0; i < n; i++) {
        user_rec_t rec;
        memset(&rec, 0, sizeof(rec));
        if (db_user_get(ids[i], &rec) != DG_OK)
            continue;                /* 枚举与读取间被并发删除:跳过该行 */
        snprintf(rows[i].user_id, sizeof(rows[i].user_id), "%s", rec.user_id);
        snprintf(rows[i].user_name, sizeof(rows[i].user_name), "%s", rec.user_name);
        rows[i].role = rec.role;
    }
    *out_n = n;
    return db_user_count(out_total);
}

int enroll_service_log_query(const log_query_t *q, log_page_t *out)
{
    return db_log_query(q, out);
}

int enroll_service_user_save(const char *user_id, const char *name,
                             int32_t role, const char *pwd)
{
    if (!user_id || !user_id[0])
        return DG_ERR_PARAM;

    user_rec_t existing;
    const int grc = db_user_get(user_id, &existing);
    if (grc != DG_OK && grc != DG_ERR_NOT_FOUND)
        return grc;                  /* DB 异常不能误判成"不存在"去建用户 */

    if (grc == DG_OK) {
        /* EDIT:完整库记录为基线覆写——db_user_update 对 ic_card/auth_flags
         * 是覆盖语义(空=解绑/清零),零基线会把没动过的字段顺手清掉 */
        user_rec_t rec = existing;
        snprintf(rec.user_name, sizeof(rec.user_name), "%s", name ? name : "");
        rec.role = role;
        if (pwd && pwd[0]) {
            const int prc = db_user_set_password(&rec, pwd);
            if (prc != DG_OK)
                return prc;
        }
        return db_user_update(&rec);
    }

    /* ADD:密码必设(硬规则,禁静默建无密用户);方式位只给密码——
     * 「位 ⇒ 已录凭据」不变式下,人脸/指纹/IC 由各自录入路径写位 */
    if (!pwd || !pwd[0])
        return DG_ERR_NO_PASSWORD;
    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.user_id, sizeof(rec.user_id), "%s", user_id);
    snprintf(rec.user_name, sizeof(rec.user_name), "%s", name ? name : "");
    rec.role = role;
    rec.auth_flags = DG_AUTH_PWD;
    const int prc = db_user_set_password(&rec, pwd);
    if (prc != DG_OK)
        return prc;
    return db_user_add(&rec);
}

int enroll_service_user_save_ex(const char *user_id, const char *name,
                                int32_t role, const char *pwd,
                                uint32_t auth_flags)
{
    if (!user_id || !user_id[0])
        return DG_ERR_PARAM;

    user_rec_t existing;
    const int grc = db_user_get(user_id, &existing);
    if (grc != DG_OK && grc != DG_ERR_NOT_FOUND)
        return grc;

    if (grc == DG_OK) {
        user_rec_t rec = existing;
        snprintf(rec.user_name, sizeof(rec.user_name), "%s", name ? name : "");
        rec.role = role;
        rec.auth_flags = auth_flags;         /* 完整目标值,storage 校验新增位 */
        if (pwd && pwd[0]) {
            const int prc = db_user_set_password(&rec, pwd);
            if (prc != DG_OK)
                return prc;
        }
        return db_user_update(&rec);
    }

    if (!pwd || !pwd[0])
        return DG_ERR_NO_PASSWORD;
    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.user_id, sizeof(rec.user_id), "%s", user_id);
    snprintf(rec.user_name, sizeof(rec.user_name), "%s", name ? name : "");
    rec.role = role;
    rec.auth_flags = auth_flags;             /* 新建用户带凭据位会被 add 拒收 */
    const int prc = db_user_set_password(&rec, pwd);
    if (prc != DG_OK)
        return prc;
    return db_user_add(&rec);
}

/* ---- web 静态图人脸录入(2026-10-04;编排见 enroll_service.h) ---- */

static struct {
    pthread_mutex_t mtx;
    bool     armed;                          /* 有在途上传,seq/uid 有效 */
    uint32_t seq;
    char     uid[DG_UID_LEN];
    uint32_t seq_next;                       /* 自增计数(时间戳同秒撞号坑) */
} s_web = { .mtx = PTHREAD_MUTEX_INITIALIZER };

bool enroll_service_upload_busy(void)
{
    pthread_mutex_lock(&s_web.mtx);
    const bool busy = s_web.armed;
    pthread_mutex_unlock(&s_web.mtx);
    return busy;
}

int enroll_service_face_upload(const char *user_id, const uint8_t *jpeg,
                               size_t len, uint32_t *seq_out)
{
    if (!user_id || !user_id[0] || !jpeg || len == 0 || len > DG_FACE_UPLOAD_MAX)
        return DG_ERR_PARAM;

    /* 后端能力前置:不支持就当场拒绝,绝不"受理后石沉大海" */
    if (!vision_service_still_supported())
        return DG_ERR_UNSUPPORTED;
    /* 用户存在性前置:无效目标当场报,别让 web 等到提取完才失败 */
    user_rec_t rec;
    const int grc = db_user_get(user_id, &rec);
    if (grc != DG_OK)
        return grc;

    pthread_mutex_lock(&s_web.mtx);
    if (s_web.armed) {
        pthread_mutex_unlock(&s_web.mtx);
        return DG_ERR_BUSY;
    }
    s_web.armed = true;
    s_web.seq_next++;                    /* 自增计数(时间戳同秒撞号坑) */
    if (s_web.seq_next == 0)             /* 回绕避开 0(seq 保留值) */
        s_web.seq_next = 1;
    s_web.seq = s_web.seq_next;
    snprintf(s_web.uid, sizeof(s_web.uid), "%s", user_id);
    const uint32_t seq = s_web.seq;
    pthread_mutex_unlock(&s_web.mtx);

    int rc = vision_service_still_put(seq, jpeg, len);
    if (rc != DG_OK) {
        pthread_mutex_lock(&s_web.mtx);
        s_web.armed = false;
        pthread_mutex_unlock(&s_web.mtx);
        return rc;
    }

    ev_capture_req_t ev;                     /* 与 CAPTURE_REQ 同形载荷 */
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.user_id, sizeof(ev.user_id), "%s", user_id);
    ev.seq = seq;
    EVENT_BUS_PUBLISH(EV_VISION_STILL_REQ, &ev);
    DG_LOGI(TAG, "web 上传受理:%s(%zuB,seq=%u)", user_id, len, seq);
    if (seq_out)
        *seq_out = seq;
    return DG_OK;
}

static void publish_result(const char *uid, int32_t kind, uint32_t seq, int err)
{
    ev_enroll_result_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.user_id, sizeof(ev.user_id), "%s", uid);
    ev.kind = kind;
    ev.seq = seq;
    ev.err = err;
    EVENT_BUS_PUBLISH(EV_ENROLL_RESULT, &ev);
}

/* 视觉特征回执:web 上传(seq 配对)= 直落库;设备拍摄 = 暂存草稿不落库 */
static int on_feature(const event_t *e, void *ud)
{
    (void)ud;
    const ev_feature_t *f = (const ev_feature_t *)e->data;

    uint8_t plain[DG_FEATURE_MAX];
    size_t len = 0;
    if (vision_service_fetch_feature(f->seq, plain, sizeof(plain), &len) != DG_OK) {
        DG_LOGE(TAG, "特征槽位 %u 已失效", f->seq);
        publish_result(f->user_id, DG_ENROLL_FACE, f->seq, DG_ERR_INTERNAL);
        return 0;
    }

    /* 采集时校验用户存在:无效目标当场报,别让用户拍完保存才失败 */
    user_rec_t existing;
    int rc = db_user_get(f->user_id, &existing);
    if (rc != DG_OK) {
        memset(plain, 0, sizeof(plain));
        publish_result(f->user_id, DG_ENROLL_FACE, f->seq, rc);
        return 0;
    }

    /* 同帧头像:后端在特征提交前已按同一 seq 入照片槽(无照片 = 槽位
     * NOT_FOUND,头像留空不算错——板上编码失败时的降级路径) */
    static uint8_t jpeg[DG_AVATAR_JPEG_MAX];   /* 总线线程独占,不占栈 */
    size_t jlen = 0;
    if (vision_service_fetch_avatar(f->seq, jpeg, sizeof(jpeg), &jlen) != DG_OK)
        jlen = 0;

    /* web 上传直落:先解除 armed(单飞闸)再走公共尾;结果按 seq 回执 */
    bool direct = false;
    pthread_mutex_lock(&s_web.mtx);
    if (s_web.armed && s_web.seq == f->seq &&
        strcmp(s_web.uid, f->user_id) == 0) {
        s_web.armed = false;
        direct = true;
    }
    pthread_mutex_unlock(&s_web.mtx);
    if (direct) {
        rc = face_commit(f->user_id, plain, len, jlen ? jpeg : NULL, jlen);
        memset(plain, 0, sizeof(plain));
        memset(jpeg, 0, sizeof(jpeg));
        publish_result(f->user_id, DG_ENROLL_FACE, f->seq, rc);
        return 0;
    }

    pthread_mutex_lock(&s_draft_mtx);
    snprintf(s_draft.uid, sizeof(s_draft.uid), "%s", f->user_id);
    memcpy(s_draft.feature, plain, len);
    s_draft.feature_len = len;
    if (jlen) {
        memcpy(s_draft.jpeg, jpeg, jlen);
        s_draft.jpeg_len = jlen;
    } else {
        s_draft.jpeg_len = 0;
    }
    pthread_mutex_unlock(&s_draft_mtx);

    memset(plain, 0, sizeof(plain));            /* 明文用后擦除 */
    memset(jpeg, 0, sizeof(jpeg));
    DG_LOGI(TAG, "capture: %s 草稿就绪(face %zuB avatar %zuB,未落库)",
            f->user_id, len, jlen);
    publish_result(f->user_id, DG_ENROLL_FACE, f->seq, DG_OK);
    return 0;
}

/* 静态图提取失败回执(后端直发):解除单飞闸并原样回执给 web */
static int on_still_fail(const event_t *e, void *ud)
{
    (void)ud;
    const ev_still_fail_t *r = (const ev_still_fail_t *)e->data;

    pthread_mutex_lock(&s_web.mtx);
    const bool mine = s_web.armed && s_web.seq == r->seq &&
                      strcmp(s_web.uid, r->user_id) == 0;
    if (mine)
        s_web.armed = false;
    pthread_mutex_unlock(&s_web.mtx);
    if (mine)
        publish_result(r->user_id, DG_ENROLL_FACE, r->seq, r->err);
    return 0;
}

/* ---- 指纹/IC 编排(2026-10-01;FINGERPRINT_AS608 / ICCARD_PROTOCOL)----
 * 指纹的模组序列全部在 fp_provider(独占 UART);本服务只翻译请求为
 * EV_FINGER_SET_MODE,进度/结果由 provider 直发 ENROLL 事件。IC 的绑定
 * 判定在本服务(查重/落库/方式位),卡片事件在这里消费。 */

static void finger_mode(int32_t mode, const char *uid, int32_t arg, uint32_t seq)
{
    ev_finger_mode_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.mode = mode;
    snprintf(ev.user_id, sizeof(ev.user_id), "%s", uid ? uid : "");
    ev.arg = arg;
    ev.seq = seq;
    EVENT_BUS_PUBLISH(EV_FINGER_SET_MODE, &ev);
}

/* 卡号绑定公共尾:写 ic_card + 开方式位(录入路径写位,spec-auth §4.2) */
static int ic_bind(const char *uid, const char *card_no)
{
    user_rec_t rec;
    if (db_user_get(uid, &rec) != DG_OK)
        return DG_ERR_NOT_FOUND;
    snprintf(rec.ic_card, sizeof(rec.ic_card), "%s", card_no);
    rec.auth_flags |= DG_AUTH_IC;
    return db_user_update(&rec);
}

/* 日志一律掩码(ICCARD_PROTOCOL §5);掩码串 "********"+末4+NUL = 13B */
static inline void ic_mask_of(const char *card_no, char out[13])
{
    iccard_mask(card_no, out, 13);
}

int enroll_service_ic_set(const char *user_id, const char *card_no)
{
    if (!user_id || !user_id[0] || !card_no || !iccard_no_valid(card_no))
        return DG_ERR_PARAM;

    user_rec_t hit;
    int rc = db_find_by_ic(card_no, &hit);
    if (rc == DG_OK) {
        /* 重绑同一张卡 = 幂等成功;他人卡 = 重复(排除自身语义同指纹) */
        rc = (strcmp(hit.user_id, user_id) == 0) ? DG_OK : DG_ERR_DUP_IC;
    } else if (rc == DG_ERR_NOT_FOUND) {
        rc = ic_bind(user_id, card_no);
    }
    char m[13];
    ic_mask_of(card_no, m);
    if (rc == DG_OK)
        DG_LOGI(TAG, "绑卡 %s <- %s", user_id, m);
    else
        DG_LOGW(TAG, "绑卡失败 %s <- %s (rc=%d)", user_id, m, rc);
    return rc;
}

int enroll_service_ic_clear(const char *user_id)
{
    if (!user_id || !user_id[0])
        return DG_ERR_PARAM;
    user_rec_t rec;
    int rc = db_user_get(user_id, &rec);
    if (rc != DG_OK)
        return rc;
    char m[13];
    ic_mask_of(rec.ic_card, m);           /* 掩码先取,清空后就没了 */
    if (rec.ic_card[0]) {                 /* 未绑卡 = 幂等成功,不空转写库 */
        rec.ic_card[0] = '\0';            /* 覆盖语义:空 = 解绑 */
        rec.auth_flags &= ~(uint32_t)DG_AUTH_IC;
        rc = db_user_update(&rec);
    }
    if (rc == DG_OK)
        DG_LOGI(TAG, "解绑 %s(原卡 %s)", user_id, m);
    else
        DG_LOGW(TAG, "解绑失败 %s (rc=%d)", user_id, rc);
    return rc;
}

/* IC 卡事件:录入态消费绑定;其余状态忽略(FSM 分支归 access_service) */
static int on_ic_card(const event_t *e, void *ud)
{
    (void)ud;
    const ev_ic_card_t *c = (const ev_ic_card_t *)e->data;
    if (!s_ic.active)
        return 0;

    char uid[DG_UID_LEN];
    uint32_t seq = s_ic.seq;
    snprintf(uid, sizeof(uid), "%s", s_ic.uid);
    ic_state_clear();
    ic_flush_req();                       /* 会话结束清缓冲(协议 §4) */

    /* 查重排除自身 + 落库(web ic_set 同一实现,日志掩码在其内) */
    int rc = enroll_service_ic_set(uid, c->card_no);
    publish_result(uid, DG_ENROLL_IC, seq, rc);
    return 0;
}

/* 删用户的指纹级联:列表快照随命令下发,模组删除异步进行,失败留痕不阻塞
 * (FINGERPRINT_AS608 §5.3:孤儿由对账暴露) */
static void finger_cascade_delete(const char *uid)
{
    int32_t pages[DG_FINGER_PAGES_MAX];
    uint32_t n = 0;
    if (db_finger_list_user(uid, pages, DG_FINGER_PAGES_MAX, &n) != DG_OK || n == 0)
        return;
    ev_finger_mode_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.mode = DG_FMODE_DELETE_USER;
    snprintf(ev.user_id, sizeof(ev.user_id), "%s", uid);
    for (uint32_t i = 0; i < n && i < DG_FINGER_PAGES_MAX; i++)
        ev.pages[i] = (uint16_t)pages[i];
    ev.page_cnt = (uint16_t)(n < DG_FINGER_PAGES_MAX ? n : DG_FINGER_PAGES_MAX);
    EVENT_BUS_PUBLISH(EV_FINGER_SET_MODE, &ev);
}

static int on_request(const event_t *e, void *ud)
{
    (void)ud;
    const ev_enroll_request_t *r = (const ev_enroll_request_t *)e->data;

    if (r->kind == DG_ENROLL_DELETE) {
        ic_state_clear();                 /* 用户即删:挂起的绑卡态作废 */
        enroll_service_discard_draft(r->user_id);   /* 用户即删:草稿不得残留 */
        finger_cascade_delete(r->user_id);
        int rc = db_user_del(r->user_id);
        const int lrc = vision_service_library_remove(r->user_id);  /* 特征库 DELETE */
        /* 同 clear_face:NOT_INIT=内存库本空;其余异常留痕(search_1n 命中
         * 时会回查 DB,已删用户不会再开门,残留只浪费检索) */
        if (lrc != DG_OK && lrc != DG_ERR_NOT_INIT)
            DG_LOGE(TAG, "delete: %s 内存特征库删除异常(%d)", r->user_id, lrc);
        publish_result(r->user_id, r->kind, r->seq, rc);
        return 0;
    }
    if (r->kind == DG_ENROLL_FACE_CLEAR) {
        int rc = enroll_service_clear_face(r->user_id);
        publish_result(r->user_id, r->kind, r->seq, rc);
        return 0;
    }
    if (r->kind == DG_ENROLL_FACE) {
        /* 向视觉服务请求抓取特征(板上走 rknn,sim 走 mock) */
        ev_capture_req_t req;
        memset(&req, 0, sizeof(req));
        snprintf(req.user_id, sizeof(req.user_id), "%s", r->user_id);
        req.seq = r->seq;
        EVENT_BUS_PUBLISH(EV_VISION_CAPTURE_REQ, &req);
        return 0;
    }
    if (r->kind == DG_ENROLL_FINGER) {
        finger_mode(DG_FMODE_ENROLL, r->user_id, 0, r->seq);
        return 0;
    }
    if (r->kind == DG_ENROLL_FINGER_CANCEL) {
        /* 切 IDLE 即取消:未落库模板由 provider 回滚(fp_provider.c) */
        finger_mode(DG_FMODE_IDLE, r->user_id, 0, r->seq);
        return 0;
    }
    if (r->kind == DG_ENROLL_FINGER_DEL) {
        finger_mode(DG_FMODE_FINGER_DEL, r->user_id, r->arg, r->seq);
        return 0;
    }
    if (r->kind == DG_ENROLL_IC) {
        snprintf(s_ic.uid, sizeof(s_ic.uid), "%s", r->user_id);
        s_ic.seq = r->seq;
        s_ic.active = true;
        ic_flush_req();                   /* 录入态切换时清缓冲(协议 §4) */
        DG_LOGI(TAG, "进入绑卡态:%s(下一张刷入的卡生效)", r->user_id);
        return 0;
    }
    if (r->kind == DG_ENROLL_IC_CANCEL) {
        if (s_ic.active) {
            ic_state_clear();
            ic_flush_req();
        }
        return 0;
    }
    if (r->kind == DG_ENROLL_IC_CLEAR) {
        int rc = enroll_service_ic_clear(r->user_id);   /* web ic_clear 同实现 */
        publish_result(r->user_id, r->kind, r->seq, rc);
        return 0;
    }
    return 0;
}

int enroll_service_start(void)
{
    if (s_sub_cnt)
        return DG_OK;
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_ENROLL_REQUEST, on_request, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_VISION_FEATURE, on_feature, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_VISION_STILL_FAIL, on_still_fail, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_IC_CARD, on_ic_card, NULL);
    DG_LOGI(TAG, "enroll 服务启动");
    return DG_OK;
}

void enroll_service_stop(void)
{
    for (int i = 0; i < s_sub_cnt; i++) {
        event_bus_unsubscribe(s_subs[i]);
        s_subs[i] = NULL;
    }
    s_sub_cnt = 0;
}
