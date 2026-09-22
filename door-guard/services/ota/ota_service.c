/*
 * ota_service.c — OTA 应用侧实现(方案见 docs/tech/OTA_PLAN.md)
 *
 * 状态机:IDLE → RECEIVING → VERIFIED / FAILED
 * 并发:单实例(BUSY);sha256 用 OpenSSL EVP 流式,不整体驻内存。
 *
 * 架构 v2 M2④ 按需线程流水线:
 *   web 连接线程(生产者)只把收到的字节投进环形缓冲;
 *   盘 I/O + 摘要 + 终态校验/落位在"按需写线程"完成——ota_begin 时创建,
 *   finish/abort/失败即退出,不留常驻线程(慢盘不再拖垮 HTTP 读循环)。
 *   进度经 EV_NET_OTA_PROGRESS 广播(≥5% 一拍 + 终态),上位机实时可见。
 * 测试可用 DG_OTA_DIR 覆盖暂存目录(默认 /var/lib/door-guard)。
 */
#include "ota_service.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"

#include <openssl/evp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "[OTA]";

/* 暂存放持久区(/tmp 是 tmpfs,重启即失——切换流程依赖跨重启读不到它没关系,
 * 但安装动作发生在运行中的系统里,放持久区更稳);bash 监听/复核见
 * board/rootfs-overlay/etc/init.d/S60doorguard */
#define OTA_DEFAULT_DIR "/var/lib/door-guard"
#define OTA_MAX_SIZE    (64u * 1024u * 1024u)   /* 包上限 64MB(方案 A 分区预留) */

static char s_dir[128] = OTA_DEFAULT_DIR;
static char s_staging[192], s_staged[192], s_staged_sha[200], s_staged_ver[200];

typedef struct {
    bool active;
    ota_manifest_t manifest;
    size_t received;                          /* 已落盘字节(写线程维护) */
    EVP_MD_CTX *md;                           /* 写线程独占 */
    /* ---- 按需写线程流水线 ---- */
    pthread_t tid;
    bool tid_valid;
    bool eof;                                 /* 生产者投递完毕 */
    bool abort;                               /* 生产者要求丢弃退出 */
    bool done;                                /* 写线程已退出(结果就绪) */
    int  result;                              /* 写线程终态(dg_err_t) */
    uint32_t last_permille;
    uint8_t pipe[OTA_PIPE_CAP];
    size_t head, tail, count;
    pthread_mutex_t mu;
    pthread_cond_t cv_space;                  /* 生产者等空位 */
    pthread_cond_t cv_data;                   /* 消费者等数据/状态变化 */
} ota_ctx_t;

static ota_ctx_t s_ctx = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv_space = PTHREAD_COND_INITIALIZER,
    .cv_data = PTHREAD_COND_INITIALIZER,
};

static int read_hex(const char *hex, uint8_t *out, size_t len)
{
    if (strlen(hex) != len * 2)
        return DG_ERR_PARAM;
    for (size_t i = 0; i < len; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1)
            return DG_ERR_PARAM;
        out[i] = (uint8_t)v;
    }
    return DG_OK;
}

static void paths_init(void)
{
    const char *env = getenv("DG_OTA_DIR");
    snprintf(s_dir, sizeof(s_dir), "%s", (env && env[0]) ? env : OTA_DEFAULT_DIR);
    snprintf(s_staging, sizeof(s_staging), "%s/ota_staging.part", s_dir);
    snprintf(s_staged, sizeof(s_staged), "%s/ota_staged.bin", s_dir);
    snprintf(s_staged_sha, sizeof(s_staged_sha), "%s/ota_staged.bin.sha256", s_dir);
    snprintf(s_staged_ver, sizeof(s_staged_ver), "%s/ota_staged.ver", s_dir);
}

static void publish_progress(uint32_t permille, bool done, int err)
{
    ev_ota_progress_t ev = { .permille = permille, .done = done, .err = err };
    EVENT_BUS_PUBLISH(EV_NET_OTA_PROGRESS, &ev);
}

static bool pipe_pop(uint8_t *out, size_t cap, size_t *got)
{
    size_t n = s_ctx.count < cap ? s_ctx.count : cap;
    if (n == 0)
        return false;
    size_t first = OTA_PIPE_CAP - s_ctx.tail;
    if (first > n)
        first = n;
    memcpy(out, s_ctx.pipe + s_ctx.tail, first);
    if (n > first)
        memcpy(out + first, s_ctx.pipe, n - first);
    s_ctx.tail = (s_ctx.tail + n) % OTA_PIPE_CAP;
    s_ctx.count -= n;
    *got = n;
    return true;
}

/* 写线程体:消费环形缓冲 → 落盘+摘要;eof 排空后校验/落位;退出前置 done */
static void *writer_thread(void *arg)
{
    (void)arg;
    uint8_t buf[16 * 1024];
    int rc = DG_OK;

    /* 续传:先把已暂存字节重放进摘要(与收包同一摘要流) */
    if (s_ctx.received > 0) {
        FILE *f = fopen(s_staging, "rb");
        if (!f) {
            rc = DG_ERR_IO;
            goto finish;
        }
        uint8_t rb[4096];
        size_t n;
        while ((n = fread(rb, 1, sizeof(rb), f)) > 0)
            EVP_DigestUpdate(s_ctx.md, rb, n);
        fclose(f);
    }

    while (rc == DG_OK) {
        pthread_mutex_lock(&s_ctx.mu);
        while (s_ctx.count == 0 && !s_ctx.eof && !s_ctx.abort)
            pthread_cond_wait(&s_ctx.cv_data, &s_ctx.mu);
        size_t got = 0;
        bool have = pipe_pop(buf, sizeof(buf), &got);
        bool eof_drained = s_ctx.eof && s_ctx.count == 0;
        pthread_mutex_unlock(&s_ctx.mu);
        pthread_cond_broadcast(&s_ctx.cv_space);

        if (s_ctx.abort) {
            rc = DG_ERR_STATE;                /* 会话被取消:丢弃退出 */
            break;
        }
        if (have) {
            FILE *f = fopen(s_staging, s_ctx.received ? "ab" : "wb");
            if (!f) {
                rc = DG_ERR_IO;
                break;
            }
            size_t w = fwrite(buf, 1, got, f);
            fclose(f);
            if (w != got) {
                rc = DG_ERR_IO;
                break;
            }
            EVP_DigestUpdate(s_ctx.md, buf, got);
            s_ctx.received += got;

            uint32_t permille =
                (uint32_t)((uint64_t)s_ctx.received * 1000 / s_ctx.manifest.size);
            if (permille - s_ctx.last_permille >= 5) {
                s_ctx.last_permille = permille;
                publish_progress(permille, false, DG_OK);
            }
        }
        if (eof_drained)
            break;
    }

    /* 终态校验 + 落位(仅正常收满才做;abort/写失败直接判负) */
    if (rc == DG_OK) {
        if (s_ctx.received != s_ctx.manifest.size) {
            DG_LOGW(TAG, "大小不符 received=%zu 声明=%u", s_ctx.received,
                    s_ctx.manifest.size);
            rc = DG_ERR_IO;                   /* 不完整:保留 .part 供续传 */
        } else {
            uint8_t digest[32], expect[32];
            unsigned dlen = 0;
            EVP_DigestFinal_ex(s_ctx.md, digest, &dlen);
            EVP_MD_CTX_free(s_ctx.md);
            s_ctx.md = NULL;
            if (read_hex(s_ctx.manifest.sha256, expect, 32) != DG_OK)
                rc = DG_ERR_PARAM;
            else if (memcmp(digest, expect, 32) != 0) {
                DG_LOGW(TAG, "sha256 不符,拒收");
                remove(s_staging);            /* 校验失败:丢弃,不保留坏包 */
                rc = DG_ERR_IO;
            } else {
                remove(s_staged);
                if (rename(s_staging, s_staged) != 0) {
                    rc = DG_ERR_IO;
                } else {
                    /* sidecar:bash 监听器做二次复核与槽位记录用(写失败不影响
                     * 闭环,应用侧 sha256 已验证通过) */
                    FILE *sf = fopen(s_staged_sha, "w");
                    if (sf) {
                        fprintf(sf, "%s  ota_staged.bin\n", s_ctx.manifest.sha256);
                        fclose(sf);
                    }
                    FILE *vf = fopen(s_staged_ver, "w");
                    if (vf) {
                        fprintf(vf, "%s", s_ctx.manifest.version);
                        fclose(vf);
                    }
                    DG_LOGI(TAG, "校验闭环通过 v=%s → %s(不刷分区,见 OTA_PLAN.md)",
                            s_ctx.manifest.version, s_staged);
                }
            }
        }
    }
    if (s_ctx.md) {                           /* 失败路径:摘要上下文补释放 */
        EVP_MD_CTX_free(s_ctx.md);
        s_ctx.md = NULL;
    }

finish:
    pthread_mutex_lock(&s_ctx.mu);
    s_ctx.result = rc;
    s_ctx.done = true;
    publish_progress(1000, true, rc == DG_OK ? DG_OK : rc);
    pthread_cond_broadcast(&s_ctx.cv_data);   /* finish/abort 的等待者 */
    pthread_cond_broadcast(&s_ctx.cv_space);  /* 满缓冲等空位的生产者 */
    pthread_mutex_unlock(&s_ctx.mu);
    return NULL;
}

int ota_begin(const ota_manifest_t *m, uint32_t offset, bool *resumed)
{
    if (!m || !resumed)
        return DG_ERR_PARAM;
    if (s_ctx.active)
        return DG_ERR_BUSY;
    if (m->size == 0 || m->size > OTA_MAX_SIZE) {
        DG_LOGW(TAG, "包大小 %u 超限[%d,%u],开始前拒绝", m->size, 0, OTA_MAX_SIZE);
        return DG_ERR_PARAM;                /* 超目标大小:开始前拒绝 */
    }

    paths_init();
    mkdir(s_dir, 0755);                     /* 已存在则忽略 */

    size_t staged = 0;
    FILE *probe = fopen(s_staging, "rb");
    if (probe) {
        fseek(probe, 0, SEEK_END);
        staged = (size_t)ftell(probe);
        fclose(probe);
    }

    *resumed = false;
    if (offset > 0) {
        /* 续传:偏移必须与已暂存严格一致 */
        if (offset != staged)
            return DG_ERR_STATE;
        *resumed = true;
    } else if (staged > 0) {
        remove(s_staging);                  /* 全新上传:清残留 */
        staged = 0;
    }
    if (staged > m->size)
        return DG_ERR_STATE;

    EVP_MD_CTX *md = EVP_MD_CTX_new();
    if (!md)
        return DG_ERR_NO_MEMORY;
    EVP_DigestInit_ex(md, EVP_sha256(), NULL);

    memset(&s_ctx.pipe, 0, sizeof(s_ctx.pipe));
    s_ctx.head = s_ctx.tail = s_ctx.count = 0;
    s_ctx.eof = s_ctx.abort = s_ctx.done = false;
    s_ctx.result = DG_OK;
    s_ctx.last_permille = 0;
    s_ctx.manifest = *m;
    s_ctx.received = staged;
    s_ctx.md = md;
    s_ctx.active = true;                    /* 会话生效:写线程的最终裁决前拒新会话 */

    /* 按需写线程:存在期 = 本次上传(写完/校验失败即退出,不留常驻) */
    if (pthread_create(&s_ctx.tid, NULL, writer_thread, NULL) != 0) {
        EVP_MD_CTX_free(md);
        s_ctx.md = NULL;
        s_ctx.active = false;
        return DG_ERR_NO_MEMORY;
    }
    s_ctx.tid_valid = true;
    pthread_detach(s_ctx.tid);

    DG_LOGI(TAG, "开始接收 v=%s size=%u offset=%zu resumed=%d(写线程已就位)",
            m->version, m->size, staged, *resumed);
    publish_progress((uint32_t)(staged * 1000 / m->size), false, DG_OK);
    return DG_OK;
}

int ota_write_chunk(const uint8_t *data, size_t len, size_t *received)
{
    if (!s_ctx.active)
        return DG_ERR_STATE;
    if (!data && len > 0)
        return DG_ERR_PARAM;

    /* 会话预检的容量上限(逐字节裁决在写线程按 manifest.size 收口) */
    pthread_mutex_lock(&s_ctx.mu);
    while (s_ctx.count == OTA_PIPE_CAP && !s_ctx.abort && !s_ctx.done) {
        /* 慢盘背压:等写线程腾空间(超时兜底防永久挂死) */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 10;
        pthread_cond_timedwait(&s_ctx.cv_space, &s_ctx.mu, &ts);
    }
    if (s_ctx.done || s_ctx.abort) {        /* 写线程已终态:把它的错误带回去 */
        int rc = s_ctx.result != DG_OK ? s_ctx.result : DG_ERR_STATE;
        pthread_mutex_unlock(&s_ctx.mu);
        return rc;
    }
    if (s_ctx.received + s_ctx.count + len > s_ctx.manifest.size) {
        pthread_mutex_unlock(&s_ctx.mu);
        return DG_ERR_PARAM;                /* 超声明大小,写途中拒绝 */
    }
    size_t first = OTA_PIPE_CAP - s_ctx.head;
    if (first > len)
        first = len;
    memcpy(s_ctx.pipe + s_ctx.head, data, first);
    if (len > first)
        memcpy(s_ctx.pipe, data + first, len - first);
    s_ctx.head = (s_ctx.head + len) % OTA_PIPE_CAP;
    s_ctx.count += len;
    size_t progress = s_ctx.received + s_ctx.count;
    pthread_mutex_unlock(&s_ctx.mu);
    pthread_cond_broadcast(&s_ctx.cv_data);

    if (received)
        *received = progress;
    return DG_OK;
}

size_t ota_can_accept(void)
{
    pthread_mutex_lock(&s_ctx.mu);
    size_t cap = s_ctx.active ? OTA_PIPE_CAP - s_ctx.count : 0;
    pthread_mutex_unlock(&s_ctx.mu);
    return cap;
}

/* 等写线程终态(带超时;期间生产者已把 eof/abort 置位) */
static int wait_writer_done(uint32_t timeout_s)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)timeout_s;
    while (!s_ctx.done) {
        int rc = pthread_cond_timedwait(&s_ctx.cv_data, &s_ctx.mu, &ts);
        if (rc != 0) {
            DG_LOGE(TAG, "等待写线程超时(%us),按失败收场", timeout_s);
            return DG_ERR_TIMEOUT;
        }
    }
    return s_ctx.result;
}

int ota_finish(char *stage_path, size_t path_cap)
{
    if (!s_ctx.active)
        return DG_ERR_STATE;

    pthread_mutex_lock(&s_ctx.mu);
    s_ctx.eof = true;                       /* 投递完毕:写线程排空后裁决 */
    pthread_cond_broadcast(&s_ctx.cv_data);
    int rc = wait_writer_done(60);
    s_ctx.tid_valid = false;
    s_ctx.active = false;
    pthread_mutex_unlock(&s_ctx.mu);

    if (rc != DG_OK)
        return rc;
    if (stage_path)
        snprintf(stage_path, path_cap, "%s", s_staged);
    return DG_OK;
}

void ota_abort(void)
{
    if (!s_ctx.active)
        return;
    pthread_mutex_lock(&s_ctx.mu);
    s_ctx.abort = true;                     /* 写线程丢弃残余并退出 */
    pthread_cond_broadcast(&s_ctx.cv_data);
    (void)wait_writer_done(10);
    s_ctx.tid_valid = false;
    s_ctx.active = false;
    pthread_mutex_unlock(&s_ctx.mu);
    DG_LOGW(TAG, "上传取消,暂存清理");
}

size_t ota_staged_bytes(void)
{
    pthread_mutex_lock(&s_ctx.mu);
    if (s_ctx.active) {                     /* 会话中:已落盘 + 在途 */
        size_t n = s_ctx.received + s_ctx.count;
        pthread_mutex_unlock(&s_ctx.mu);
        return n;
    }
    pthread_mutex_unlock(&s_ctx.mu);
    /* 非会话态:读暂存文件(续传对拍的真值) */
    paths_init();
    FILE *probe = fopen(s_staging, "rb");
    if (!probe)
        return 0;
    fseek(probe, 0, SEEK_END);
    long n = ftell(probe);
    fclose(probe);
    return n > 0 ? (size_t)n : 0;
}
