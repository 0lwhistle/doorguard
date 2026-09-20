/*
 * test_ota.c — OTA 按需写线程流水线测试(M2④;宿主可跑,DG_OTA_DIR 指向临时目录)
 *
 * 覆盖:正常接收→校验闭环→落位;sha256 不符拒收;超声明大小拒绝;
 * 单实例 BUSY;中途 abort 后可重新开始;断点续传(offset 对拍);
 * EV_NET_OTA_PROGRESS 进度/终态事件真的在发(M2④ 前该事件从未被发布)。
 */
#include "dg_test.h"
#include "ota_service.h"
#include "event_bus.h"
#include "events.h"

#include <openssl/evp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONTENT_SZ 100000u

static char s_dir[64];
static uint8_t s_content[CONTENT_SZ];
static char s_sha_hex[65];

/* ---- 事件捕获(总线异步分发:计数 + 终态标志) ---- */
static pthread_mutex_t s_ev_mu = PTHREAD_MUTEX_INITIALIZER;
static int s_ev_count;
static bool s_ev_terminal;
static int s_ev_term_err;

static int on_ota_progress(const event_t *e, void *ud)
{
    (void)ud;
    const ev_ota_progress_t *ev = (const ev_ota_progress_t *)e->data;
    pthread_mutex_lock(&s_ev_mu);
    s_ev_count++;
    if (ev->done) {
        s_ev_terminal = true;
        s_ev_term_err = ev->err;
    }
    pthread_mutex_unlock(&s_ev_mu);
    return 0;
}

static bool wait_terminal(int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 10; i++) {
        pthread_mutex_lock(&s_ev_mu);
        bool t = s_ev_terminal;
        pthread_mutex_unlock(&s_ev_mu);
        if (t)
            return true;
        usleep(10 * 1000);
    }
    return false;
}

static void sha256_hex(const uint8_t *data, size_t len, char *out /*65*/)
{
    uint8_t d[32];
    unsigned dlen = 0;
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    EVP_DigestInit_ex(md, EVP_sha256(), NULL);
    EVP_DigestUpdate(md, data, len);
    EVP_DigestFinal_ex(md, d, &dlen);
    EVP_MD_CTX_free(md);
    for (int i = 0; i < 32; i++)
        sprintf(out + i * 2, "%02x", d[i]);
}

static void manifest_make(ota_manifest_t *m, uint32_t size, const char *sha)
{
    memset(m, 0, sizeof(*m));
    snprintf(m->version, sizeof(m->version), "v-test-1");
    m->size = size;
    snprintf(m->sha256, sizeof(m->sha256), "%s", sha);
}

/* 投递 [from,to) 字节(单次 chunk 可大于写线程单批,验证环形缓冲绕回) */
static int push_all(const uint8_t *data, size_t from, size_t to)
{
    size_t got = 0;
    int rc = ota_write_chunk(data + from, to - from, &got);
    if (rc == DG_OK)
        DG_CHECK(got == to);
    return rc;
}

int main(void)
{
    snprintf(s_dir, sizeof(s_dir), "/tmp/dg_ota_%d", (int)getpid());
    char cmd[192];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", s_dir, s_dir);
    if (system(cmd) != 0)
        exit(1);
    setenv("DG_OTA_DIR", s_dir, 1);

    for (uint32_t i = 0; i < CONTENT_SZ; i++)
        s_content[i] = (uint8_t)(i * 7 + (i >> 8));
    sha256_hex(s_content, CONTENT_SZ, s_sha_hex);

    DG_CHECK(event_bus_init() == EVENT_BUS_OK);
    event_subscription_t *sub =
        event_bus_subscribe(EV_NET_OTA_PROGRESS, on_ota_progress, NULL);
    DG_CHECK(sub != NULL);

    char staged[192];
    snprintf(staged, sizeof(staged), "%s/ota_staged.bin", s_dir);

    /* ---- O1 正常闭环:接收→校验→落位→进度/终态事件 ---- */
    printf("[O1] happy path: receive + verify + staged + events\n");
    ota_manifest_t m;
    manifest_make(&m, CONTENT_SZ, s_sha_hex);
    bool resumed = false;
    DG_CHECK(ota_begin(&m, 0, &resumed) == DG_OK);
    DG_CHECK(!resumed);
    DG_CHECK(push_all(s_content, 0, 40000) == DG_OK);
    DG_CHECK(push_all(s_content, 40000, 80000) == DG_OK);
    DG_CHECK(push_all(s_content, 80000, CONTENT_SZ) == DG_OK);
    char path[192];
    DG_CHECK(ota_finish(path, sizeof(path)) == DG_OK);
    DG_CHECK(strcmp(path, staged) == 0);
    DG_CHECK(access(staged, F_OK) == 0);
    DG_CHECK(wait_terminal(5000));
    pthread_mutex_lock(&s_ev_mu);
    DG_CHECK(s_ev_count >= 2);              /* 至少:进度一拍 + 终态 */
    DG_CHECK(s_ev_term_err == DG_OK);
    int cnt_after_o1 = s_ev_count;
    pthread_mutex_unlock(&s_ev_mu);

    /* ---- O2 单实例 BUSY(会话结束后可重新开始) ---- */
    printf("[O2] busy during session, reusable after\n");
    manifest_make(&m, CONTENT_SZ, s_sha_hex);
    DG_CHECK(ota_begin(&m, 0, &resumed) == DG_OK);
    DG_CHECK(ota_begin(&m, 0, &resumed) == DG_ERR_BUSY);
    DG_CHECK(push_all(s_content, 0, 1000) == DG_OK);
    DG_CHECK(ota_finish(path, sizeof(path)) == DG_ERR_IO);   /* 不完整:拒收 */
    DG_CHECK(ota_begin(&m, 0, &resumed) == DG_OK);           /* 会话已结束 */
    ota_abort();

    /* ---- O3 sha256 不符:拒收,无落位,终态错误事件 ---- */
    printf("[O3] sha mismatch rejected\n");
    manifest_make(&m, CONTENT_SZ,
                  "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef");
    remove(staged);                         /* 清掉 O1 的落位文件,拒收后应保持不存在 */
    DG_CHECK(ota_begin(&m, 0, &resumed) == DG_OK);
    DG_CHECK(push_all(s_content, 0, CONTENT_SZ) == DG_OK);
    DG_CHECK(ota_finish(path, sizeof(path)) == DG_ERR_IO);
    DG_CHECK(access(staged, F_OK) != 0);
    DG_CHECK(wait_terminal(5000));
    pthread_mutex_lock(&s_ev_mu);
    DG_CHECK(s_ev_term_err == DG_ERR_IO);
    pthread_mutex_unlock(&s_ev_mu);

    /* ---- O4 超声明大小:写途中拒绝 ---- */
    printf("[O4] oversize rejected mid-stream\n");
    manifest_make(&m, 1000, s_sha_hex);
    DG_CHECK(ota_begin(&m, 0, &resumed) == DG_OK);
    DG_CHECK(push_all(s_content, 0, 2000) == DG_ERR_PARAM);
    ota_abort();

    /* ---- O5 断点续传:手工造 .part → offset 对拍 → 补齐闭环 ---- */
    printf("[O5] resume from staged part\n");
    const size_t PART = 30000;
    char part[192];
    snprintf(part, sizeof(part), "%s/ota_staging.part", s_dir);
    FILE *f = fopen(part, "wb");
    if (!f)
        exit(1);
    fwrite(s_content, 1, PART, f);
    fclose(f);
    manifest_make(&m, CONTENT_SZ, s_sha_hex);
    DG_CHECK(ota_begin(&m, PART, &resumed) == DG_OK);
    DG_CHECK(resumed);
    DG_CHECK(push_all(s_content, PART, CONTENT_SZ) == DG_OK);
    DG_CHECK(ota_finish(path, sizeof(path)) == DG_OK);
    DG_CHECK(access(staged, F_OK) == 0);

    /* 偏移与已暂存不符:显式拒绝 */
    DG_CHECK(ota_begin(&m, 12345, &resumed) == DG_ERR_STATE);

    /* 收尾:事件总数只增不减;清理 */
    pthread_mutex_lock(&s_ev_mu);
    DG_CHECK(s_ev_count >= cnt_after_o1);
    pthread_mutex_unlock(&s_ev_mu);

    event_bus_unsubscribe(sub);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", s_dir);
    if (system(cmd) != 0) { /* 清理失败不影响结论 */ }

    DG_TEST_EXIT();
}
