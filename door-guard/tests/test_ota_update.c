/*
 * test_ota_update.c — MQTT OTA 升级编排测试(宿主;传输注入,零网络依赖)
 *
 * 覆盖:版本比较规则表 / 公告解析(缺字段/坏 sha/坏 size 丢弃)/
 * 旧公告忽略 / 无通道查询报 NETWORK / 手动流(公告→AVAILABLE→apply→
 * DOWNLOADING→STAGED,暂存文件落位,事件序列齐全)/ sha 不符拒收 FAILED
 * 且不落 staged / 公告 size 与源对拍 / 缺 url 失败 / 慢源在途重入 BUSY。
 * DG_OTA_DIR 指向临时目录(与 test_ota 同款),事件经总线订阅捕获。
 */
#include "dg_test.h"
#include "ota/ota_update.h"
#include "event_bus.h"
#include "events.h"

#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONTENT_SZ 1000u

static char s_dir[64];
static uint8_t s_content[CONTENT_SZ];

/* ---- 事件捕获(总线线程异步;计数 + 最近一拍) ---- */

#define EV_CAP 64
static ev_ota_update_t s_evs[EV_CAP];
static volatile int s_ev_cnt;

static int on_ota_update(const event_t *e, void *ud)
{
    (void)ud;
    const ev_ota_update_t *ev = (const ev_ota_update_t *)e->data;
    int i = s_ev_cnt < EV_CAP ? s_ev_cnt : EV_CAP - 1;
    s_evs[i] = *ev;
    s_ev_cnt++;
    return 0;
}

static void ev_reset(void)
{
    s_ev_cnt = 0;
    memset(s_evs, 0, sizeof(s_evs));
}

static bool ev_saw(ota_upd_state_t st)
{
    for (int i = 0; i < s_ev_cnt && i < EV_CAP; i++)
        if (s_evs[i].state == st)
            return true;
    return false;
}

static void wait_state(ota_upd_state_t want, int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 5; i++) {
        ota_upd_status_t st;
        ota_update_status(&st);
        if (st.state == want)
            return;
        usleep(5000);
    }
}


/* 事件异步分发(总线线程):终态到位后再等事件落袋,避免 ev_saw 竞态 */
static void wait_events(int n, int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 5; i++) {
        if (s_ev_cnt >= n)
            return;
        usleep(5000);
    }
}

/* ---- 传输注入:内存源 ---- */

typedef struct {
    const uint8_t *data;
    size_t len, pos;
    int open_calls;
    uint32_t declared_len;                   /* open 回报的 content-length */
} mem_src_t;

static mem_src_t s_mem;

static int mem_open(void *ud, const char *url, uint32_t *content_len)
{
    (void)url;
    mem_src_t *m = (mem_src_t *)ud;
    m->open_calls++;
    *content_len = m->declared_len;
    m->pos = 0;
    return DG_OK;
}

static int mem_read(void *ud, uint8_t *buf, size_t cap, size_t *got)
{
    mem_src_t *m = (mem_src_t *)ud;
    size_t left = m->len - m->pos;
    size_t n = left < cap ? left : cap;
    memcpy(buf, m->data + m->pos, n);
    m->pos += n;
    *got = n;
    return DG_OK;
}

static void mem_close(void *ud)
{
    (void)ud;
}

static const ota_transport_t MEM_TP = { mem_open, mem_read, mem_close };

/* 慢源:读到一半卡闸(s_gate 放行),制造「在途」窗口测 BUSY 重入 */
static volatile int s_gate;
static int gate_read(void *ud, uint8_t *buf, size_t cap, size_t *got)
{
    mem_src_t *m = (mem_src_t *)ud;
    while (m->pos >= m->len / 2 && !s_gate)
        usleep(2000);
    return mem_read(ud, buf, cap, got);
}

static const ota_transport_t GATE_TP = { mem_open, gate_read, mem_close };

/* ---- 公告模板 ---- */

static char s_ann_json[512];

static void make_ann(const char *version, const char *sha, long long size,
                     const char *url)
{
    snprintf(s_ann_json, sizeof(s_ann_json),
             "{\"version\":\"%s\",\"date\":\"2026-10-05\",\"url\":\"%s\","
             "\"sha256\":\"%s\",\"size\":%lld,\"notes\":\"test notes\"}",
             version, url ? url : "", sha, size);
}

/* ---- sha256(EVP 流式;与 ota_service 校验同源口径) ---- */

static void make_sha(char *out, size_t cap, const uint8_t *data, size_t len)
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdl = 0;
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    EVP_DigestInit_ex(c, EVP_sha256(), NULL);
    EVP_DigestUpdate(c, data, len);
    EVP_DigestFinal_ex(c, md, &mdl);
    EVP_MD_CTX_free(c);
    for (unsigned i = 0; i < mdl && (size_t)(i * 2 + 1) < cap; i++)
        snprintf(out + i * 2, 3, "%02x", md[i]);
    out[mdl * 2] = '\0';
}

int main(void)
{
    /* DG_OTA_DIR 必须在任何 ota_service 调用前就位(paths_init 取环境) */
    snprintf(s_dir, sizeof(s_dir), "/tmp/dg_ota_upd_%d", (int)getpid());
    DG_CHECK(setenv("DG_OTA_DIR", s_dir, 1) == 0);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", s_dir);
    DG_CHECK(system(cmd) >= 0);

    for (size_t i = 0; i < CONTENT_SZ; i++)
        s_content[i] = (uint8_t)(i * 31 + 7);
    char sha[65];
    make_sha(sha, sizeof(sha), s_content, CONTENT_SZ);

    event_bus_subscribe(EV_NET_OTA_UPDATE, on_ota_update, NULL);
    ota_update_transport_set(&MEM_TP, &s_mem);
    ota_update_start();

    /* [T1] 版本比较规则表 */
    printf("[T1] 版本比较\n");
    DG_CHECK(ota_version_newer("unknown", "1.0.0") == true);
    DG_CHECK(ota_version_newer("1.0.0", "1.0.1") == true);
    DG_CHECK(ota_version_newer("1.0.0", "1.1.0") == true);
    DG_CHECK(ota_version_newer("1.0.0", "2.0.0") == true);
    DG_CHECK(ota_version_newer("v1.0.0", "1.0.0") == false);        /* 相等 */
    DG_CHECK(ota_version_newer("1.0.0", "v1.0.0") == false);
    DG_CHECK(ota_version_newer("1.0.0-12-gabc", "1.0.0") == false); /* 后缀不比 */
    DG_CHECK(ota_version_newer("1.0.0-12-gabc", "1.0.1") == true);
    DG_CHECK(ota_version_newer("1.10.0", "1.9.0") == false);        /* 数值非字典序 */
    DG_CHECK(ota_version_newer("1.0.0", "") == false);
    DG_CHECK(ota_version_newer("1.0.0", "garbage") == false);

    /* [T2] 公告解析:坏 sha / size<=0 / 缺 version 全丢弃,状态保持 IDLE */
    printf("[T2] 公告解析\n");
    ev_reset();
    make_ann("9.9.1", "zz", CONTENT_SZ, "http://x/f.bin");
    ota_update_on_announce(s_ann_json);
    DG_CHECK(s_ev_cnt == 0);
    make_ann("9.9.1", sha, 0, "http://x/f.bin");
    ota_update_on_announce(s_ann_json);
    DG_CHECK(s_ev_cnt == 0);
    ota_update_on_announce("{\"sha256\":\"\"}");
    DG_CHECK(s_ev_cnt == 0);
    ota_upd_status_t st;
    ota_update_status(&st);
    DG_CHECK(st.state == OTA_UPD_IDLE);

    /* [T3] 旧公告忽略。当前版本 = git describe(test 构建无数字前缀 tag
     * 时不可解析,规则=可解析公告一律视为新)——两种现状都钉住 */
    printf("[T3] skip-ignore"); putchar(10);
    make_ann("0.0.0", sha, CONTENT_SZ, "http://x/f.bin");
    ev_reset();
    ota_update_on_announce(s_ann_json);
    ota_update_status(&st);
    if (ota_version_newer(ota_update_current_version(), "0.0.0")) {
        DG_CHECK(st.state == OTA_UPD_AVAILABLE);   /* 不可解析:可解析即新 */
    } else {
        DG_CHECK(s_ev_cnt == 0);
        DG_CHECK(st.state == OTA_UPD_IDLE);
    }

    /* [T4] 查询无 mqtt:NETWORK */
    printf("[T4] 检查更新无通道\n");
    DG_CHECK(ota_update_check() == DG_ERR_NETWORK);

    /* [T5] 手动流:公告 → AVAILABLE(信息齐)→ apply → STAGED 落位 */
    printf("[T5] 手动下载流\n");
    ev_reset();
    memset(&s_mem, 0, sizeof(s_mem));
    s_mem.data = s_content;
    s_mem.len = CONTENT_SZ;
    s_mem.declared_len = CONTENT_SZ;
    make_ann("9.9.1", sha, CONTENT_SZ, "http://x/f.bin");
    ota_update_on_announce(s_ann_json);
    ota_update_status(&st);
    DG_CHECK(st.state == OTA_UPD_AVAILABLE);
    DG_CHECK(strcmp(st.version, "9.9.1") == 0);
    DG_CHECK(strcmp(st.date, "2026-10-05") == 0);
    DG_CHECK(strcmp(st.notes, "test notes") == 0);
    DG_CHECK(s_mem.open_calls == 0);            /* 自动更新默认关,没下载 */

    DG_CHECK(ota_update_apply() == DG_OK);
    DG_CHECK(ota_update_apply() == DG_ERR_BUSY); /* 在途重入 */
    wait_state(OTA_UPD_STAGED, 8000);
    ota_update_status(&st);
    DG_CHECK(st.state == OTA_UPD_STAGED);
    usleep(30 * 1000);   /* worker 收尾窗口:s_busy 清零后再进下一例 */

    char path[128];
    snprintf(path, sizeof(path), "%s/ota_staged.bin", s_dir);
    FILE *f = fopen(path, "rb");
    DG_CHECK(f != NULL);
    uint8_t back[CONTENT_SZ];
    DG_CHECK(f && fread(back, 1, CONTENT_SZ, f) == CONTENT_SZ);
    if (f)
        fclose(f);
    DG_CHECK(memcmp(back, s_content, CONTENT_SZ) == 0);
    wait_events(3, 3000);
    DG_CHECK(ev_saw(OTA_UPD_AVAILABLE) && ev_saw(OTA_UPD_DOWNLOADING) &&
             ev_saw(OTA_UPD_STAGED));
    DG_CHECK(s_mem.open_calls == 1);

    /* [T6] sha 不符:FAILED 且 staged 不落位(源内容与声明 sha 错开一字节) */
    printf("[T6] sha 不符拒收\n");
    snprintf(cmd, sizeof(cmd), "rm -f %s/ota_staged*", s_dir);
    DG_CHECK(system(cmd) >= 0);
    ev_reset();
    static uint8_t s_bad[CONTENT_SZ];
    memcpy(s_bad, s_content, CONTENT_SZ);
    s_bad[0] ^= 0xFF;
    memset(&s_mem, 0, sizeof(s_mem));
    s_mem.data = s_bad;
    s_mem.len = CONTENT_SZ;
    s_mem.declared_len = CONTENT_SZ;
    make_ann("9.9.2", sha, CONTENT_SZ, "http://x/f.bin");
    ota_update_on_announce(s_ann_json);
    ota_update_status(&st);
    DG_CHECK(st.state == OTA_UPD_AVAILABLE);
    DG_CHECK(ota_update_apply() == DG_OK);
    wait_state(OTA_UPD_FAILED, 8000);
    ota_update_status(&st);
    DG_CHECK(st.state == OTA_UPD_FAILED);
    snprintf(path, sizeof(path), "%s/ota_staged.bin", s_dir);
    DG_CHECK(access(path, F_OK) != 0);
    usleep(30 * 1000);   /* worker 收尾窗口:s_busy 清零后再进下一例 */

    /* [T7] 公告 size 与源大小不符:开流后立即拒绝 */
    printf("[T7] 大小对拍\n");
    ev_reset();
    memset(&s_mem, 0, sizeof(s_mem));
    s_mem.data = s_content;
    s_mem.len = CONTENT_SZ;
    s_mem.declared_len = CONTENT_SZ + 1;
    make_ann("9.9.3", sha, CONTENT_SZ, "http://x/f.bin");
    ota_update_on_announce(s_ann_json);
    DG_CHECK(ota_update_apply() == DG_OK);
    wait_state(OTA_UPD_FAILED, 8000);
    ota_update_status(&st);
    DG_CHECK(st.state == OTA_UPD_FAILED);
    usleep(30 * 1000);   /* worker 收尾窗口:s_busy 清零后再进下一例 */

    /* [T8] 缺 url(公告与 cfg 均空):PARAM 失败 */
    printf("[T8] 缺 url\n");
    ev_reset();
    memset(&s_mem, 0, sizeof(s_mem));
    s_mem.data = s_content;
    s_mem.len = CONTENT_SZ;
    make_ann("9.9.4", sha, CONTENT_SZ, "");
    ota_update_on_announce(s_ann_json);
    DG_CHECK(ota_update_apply() == DG_OK);
    wait_state(OTA_UPD_FAILED, 8000);
    ota_update_status(&st);
    DG_CHECK(st.state == OTA_UPD_FAILED && st.err == DG_ERR_PARAM);

    /* [T9] 慢源:worker 卡在半程时二次 apply 被 BUSY 拒;放行后闭环 */
    printf("[T9] 慢源重入\n");
    snprintf(cmd, sizeof(cmd), "rm -f %s/ota_staged*", s_dir);
    DG_CHECK(system(cmd) >= 0);
    ev_reset();
    s_gate = 0;
    memset(&s_mem, 0, sizeof(s_mem));
    s_mem.data = s_content;
    s_mem.len = CONTENT_SZ;
    s_mem.declared_len = CONTENT_SZ;
    ota_update_transport_set(&GATE_TP, &s_mem);
    make_ann("9.9.5", sha, CONTENT_SZ, "http://x/f.bin");
    ota_update_on_announce(s_ann_json);
    DG_CHECK(ota_update_apply() == DG_OK);
    usleep(100 * 1000);                          /* 让 worker 走到闸口 */
    DG_CHECK(ota_update_apply() == DG_ERR_BUSY);
    s_gate = 1;
    usleep(30 * 1000);
    wait_state(OTA_UPD_STAGED, 8000);
    ota_update_status(&st);
    DG_CHECK(st.state == OTA_UPD_STAGED);

    ota_update_stop();
    ota_update_transport_set(NULL, NULL);
    DG_TEST_EXIT();
}
