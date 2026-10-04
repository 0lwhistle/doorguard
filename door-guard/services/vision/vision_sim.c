/*
 * vision_sim.c — PC mock 视觉后端(原 sim/sim_vision.c 收编为服务后端)
 *
 * 1. 周期 mock(可选,DG_SIM_VISION=0 关):人脸出现→命中(user 10001)/
 *    超时失败交替,驱动主页全流程演示;
 * 2. 录入抓取:响应 EV_VISION_CAPTURE_REQ,提交确定性伪特征
 *    (user_id 哈希派生:同人同特征 → 重录查重命中;异人不同特征)。
 *
 * 契约实现见文件尾 vision_backend_sim:model_tag 留空(伪特征没有"口径"一说,
 * 服务层因此不做口径校验),库维护不提供(PC 只验 UI/FSM 链路)。
 */
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"
#include "modules/jpeg/dg_jpeg.h"
#include "vision_service.h"
#include "vision_backend.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static bool s_started = false;

/* ---- 周期 mock 线程 ----
 * 按工作模式产出(与板上后端同语义,这样 PC 也能把验证流程走通):
 *   IDLE       不产出(板上=不推帧)
 *   DETECT_ONLY 只发脸框(菜单/待机/验证子步:不检索,不会误开门)
 *   DETECT_1N  脸框 + 命中/离开交替(普通模式演示)
 *   VERIFY_11  脸框 + 对目标用户发 1:1 通过(无模式分支的话 PC 验不了验证按钮)
 */
static void *mock_thread(void *arg)
{
    (void)arg;
    bool hit_next = true;
    int tick = 0;

    for (;;) {
        dg_vision_mode_t mode = vision_service_get_mode();
        tick++;

        if (mode == DG_VMODE_IDLE) {
            usleep(500 * 1000);
            continue;
        }

        /* 脸框:每 3 tick(≈1.5s)发一次,模拟检测节流 */
        if (tick % 3 == 1) {
            ev_face_box_t box = { .x = 200, .y = 250, .w = 320, .h = 420,
                                  .state = DG_BOX_DETECTED };
            EVENT_BUS_PUBLISH(EV_VISION_FACE_BOX, &box);
        }

        /* 结果:每 8 tick(≈4s)一轮,与真机的判定节奏接近 */
        if (tick % 8 == 0) {
            if (mode == DG_VMODE_DETECT_1N) {
                if (hit_next) {
                    ev_match_t m;
                    memset(&m, 0, sizeof(m));
                    m.matched = true;
                    snprintf(m.user_id, sizeof(m.user_id), "%s", "10001");
                    snprintf(m.user_name, sizeof(m.user_name), "%s", "张三");
                    m.role = DG_ROLE_NORMAL;
                    m.score_permille = 952;
                    EVENT_BUS_PUBLISH(EV_VISION_MATCH_1N, &m);
                } else {
                    EVENT_BUS_PUBLISH_EMPTY(EV_VISION_FACE_LOST);
                }
                hit_next = !hit_next;
            } else if (mode == DG_VMODE_VERIFY_11) {
                char uid[DG_UID_LEN];
                vision_service_get_verify_uid(uid, sizeof(uid));
                if (uid[0]) {                       /* 有目标才"比中" */
                    ev_match_t m;
                    memset(&m, 0, sizeof(m));
                    m.matched = true;
                    snprintf(m.user_id, sizeof(m.user_id), "%s", uid);
                    snprintf(m.user_name, sizeof(m.user_name), "%s", "模拟用户");
                    m.role = DG_ROLE_NORMAL;
                    m.score_permille = 930;
                    EVENT_BUS_PUBLISH(EV_VISION_VERIFY_11, &m);
                }
            }
            /* DETECT_ONLY:只画框,不出结果 */
        }

        usleep(500 * 1000);
    }
    return NULL;
}

/* ---- 录入抓取:确定性伪特征(演示/测试用) ----
 * 拍摄(EV_VISION_CAPTURE_REQ)与静态图上传(EV_VISION_STILL_REQ)共用
 * 同一提交尾:伪特征按 user_id 哈希派生(同人恒定 → 查重可判定),伪头像
 * 是同 seed 渐变图编码的真 JPEG;seq 原样回传(请求-回执配对语义) */

static void sim_submit_mock(const char *user_id, uint32_t seq)
{
    /* 伪特征 = user_id 字节重复展开(同人恒定,异人不同;查重可判定) */
    uint8_t feat[64];
    size_t len = 32;
    uint8_t seed = 0;
    for (const char *p = user_id; *p; p++)
        seed ^= (uint8_t)*p;
    memset(feat, seed, sizeof(feat));

    /* static:总线分发线程栈仅 64KB,大数组上栈会溢出(与板上后端同纪律) */
    static uint8_t rgb[64 * 64 * 3];
    static uint8_t jpeg[DG_AVATAR_JPEG_MAX];
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            uint8_t *px = &rgb[(y * 64 + x) * 3];
            px[0] = (uint8_t)(seed + x * 3);
            px[1] = (uint8_t)(seed + y * 3);
            px[2] = (uint8_t)(seed + x + y);
        }
    size_t jlen = 0;
    if (dg_jpeg_encode_rgb(rgb, 64, 64, 80, jpeg, sizeof(jpeg), &jlen) == DG_OK)
        vision_service_put_avatar(seq, jpeg, jlen);
    else
        DG_LOGW("[VISION]", "sim 头像编码失败(忽略:测试环境异常)");

    vision_service_submit_feature(user_id, seq, feat, len);
}

static int on_capture_req(const event_t *e, void *ud)
{
    (void)ud;
    const ev_capture_req_t *r = (const ev_capture_req_t *)e->data;
    sim_submit_mock(r->user_id, r->seq);
    return 0;
}

/* 静态图上传:与拍摄同 mock 提交(上传图内容本身不参与伪特征派生;
 * 宿主端到端只验"受理→提取→直落库→回执"链路,不验图像语义) */
static int on_still_req(const event_t *e, void *ud)
{
    (void)ud;
    static uint8_t jpg[DG_FACE_UPLOAD_MAX];   /* 总线线程栈小,静态接图 */
    size_t len = 0;
    const ev_capture_req_t *r = (const ev_capture_req_t *)e->data;
    if (vision_service_still_fetch(r->seq, jpg, sizeof(jpg), &len) != DG_OK) {
        DG_LOGW("[VISION]", "sim 静态图槽取件失败(seq=%u)", r->seq);
        return 0;
    }
    DG_LOGI("[VISION]", "sim 静态图录入 %s(%zu B,seq=%u)", r->user_id, len, r->seq);
    sim_submit_mock(r->user_id, r->seq);
    return 0;
}

/* ---- 内存特征库(PC 版) ----
 * 2026-09-28:enroll commit 改严格事务序后,后端必须提供库操作(返回值被
 * 检查,NULL ops 会让 PC/sim 路径的录入保存必然失败)。库内容只做
 * 增删记账——sim 的假 1:N 命中逻辑与库无关(构造性假数据),但
 * 「DB 与内存库两侧一致」的契约要与板上后端同构 */
#define SIM_LIB_MAX 64
static struct {
    pthread_mutex_t mtx;
    int n;
    char uid[SIM_LIB_MAX][DG_UID_LEN];
    uint8_t vec[SIM_LIB_MAX][DG_FEATURE_MAX];
    uint16_t len[SIM_LIB_MAX];
} s_sim_lib = { .mtx = PTHREAD_MUTEX_INITIALIZER };

static int sim_lib_del(const char *user_id);   /* add 的覆盖语义先删后加 */

static int sim_lib_add(const char *user_id, const uint8_t *feature, uint16_t len)
{
    if (!user_id || !user_id[0] || !feature || len == 0 || len > DG_FEATURE_MAX)
        return DG_ERR_PARAM;
    sim_lib_del(user_id);                      /* 同 uid = 覆盖(幂等) */
    pthread_mutex_lock(&s_sim_lib.mtx);
    int rc = DG_ERR_NO_MEMORY;
    if (s_sim_lib.n < SIM_LIB_MAX) {
        snprintf(s_sim_lib.uid[s_sim_lib.n], DG_UID_LEN, "%s", user_id);
        memcpy(s_sim_lib.vec[s_sim_lib.n], feature, len);
        s_sim_lib.len[s_sim_lib.n] = len;
        s_sim_lib.n++;
        rc = DG_OK;
    }
    pthread_mutex_unlock(&s_sim_lib.mtx);
    return rc;
}

static int sim_lib_del(const char *user_id)
{
    pthread_mutex_lock(&s_sim_lib.mtx);
    for (int i = 0; i < s_sim_lib.n; i++) {
        if (strcmp(s_sim_lib.uid[i], user_id) == 0) {
            s_sim_lib.n--;
            if (i != s_sim_lib.n) {            /* 尾行补位,保持紧凑(与 rknn 同款) */
                memcpy(s_sim_lib.uid[i], s_sim_lib.uid[s_sim_lib.n], DG_UID_LEN);
                memcpy(s_sim_lib.vec[i], s_sim_lib.vec[s_sim_lib.n], DG_FEATURE_MAX);
                s_sim_lib.len[i] = s_sim_lib.len[s_sim_lib.n];
            }
            break;
        }
    }
    pthread_mutex_unlock(&s_sim_lib.mtx);
    return DG_OK;                              /* 删不存在 = 成功(与 rknn 契约一致) */
}

/* 后端私有启动:PC 恒成功(mock 与摄像头 sim 解耦;工作模式由服务层持有,
 * 本后端不按模式分支:PC 只验证 UI/FSM 链路,人脸口径以板上 ROCKIVA 为准) */
static int sim_start(bool enable_mock)
{
    if (s_started)
        return DG_OK;
    s_started = true;

    /* 录入抓取订阅:bus 分发线程回调,无需独立线程 */
    event_bus_subscribe(EV_VISION_CAPTURE_REQ, on_capture_req, NULL);
    event_bus_subscribe(EV_VISION_STILL_REQ, on_still_req, NULL);

    if (enable_mock) {
        pthread_t tid;
        if (pthread_create(&tid, NULL, mock_thread, NULL) == 0)
            pthread_detach(tid);
        DG_LOGI("[VISION]", "mock 周期事件已启动");
    }
    return DG_OK;
}

/* 后端注册项(装配层 app/main.c 在 DG_SIM 构建下注册) */
const vision_backend_ops_t vision_backend_sim = {
    .name = "sim",
    .model_tag = NULL,          /* 伪特征无口径:服务层不校验 */
    .has_landmarks = false,     /* 不做活体(PC 只验链路) */
    .has_still_enroll = true,   /* 静态图上传走同一 mock 提交尾 */
    .start = sim_start,
    .lib_add = sim_lib_add,
    .lib_del = sim_lib_del,
    .compare = NULL,            /* NULL = storage 默认逐字节相等 */
    .on_mode = NULL,
};
