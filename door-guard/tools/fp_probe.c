/*
 * fp_probe.c — AS608 协议级探针(板上跑,不进 door-guard 主程序)
 *
 * 为什么单独一个可执行:应用(provider)独占 uart,协议里几条**未冻结**的指令
 * 必须脱离业务序列才能干净地实测——尤其是 UpChar/DownChar 的数据包(0x02)与
 * 结束包(0x08)帧式(FINGERPRINT_PROTOCOL §6.4 一直挂在"待首次录入"),以及
 * LoadChar 打空页时到底回什么确认码(对账要能区分"空页"与"读库错")。
 *
 * 用法(默认 /dev/ttyS8 57600):
 *   fp_probe [-d /dev/ttyS8] [-b 57600] <子命令>
 *     verify                     握手(VerifyPSW),打印应答原字节
 *     count                      ValidTempleteNum
 *     scan [from] [to]           逐页 LoadChar 探占用(默认 0..999);打印空页确认码
 *     dump <page> <file>         LoadChar+UpChar 读模板 → 写 512B 到文件
 *     put <file> <page>          DownChar+Store 写模板 → 再 LoadChar+UpChar 读回逐字节比对
 *     del <page> [count]         DeletChar
 *     empty                      Empty
 *
 * 退出码:0 成功;1 链路/协议失败;2 参数错;3 比对不一致。
 * 注意:运行前必须停掉 door-guard(它独占 /dev/ttyS8):
 *     /etc/init.d/S60doorguard stop && fp_probe ... && /etc/init.d/S60doorguard start
 */
#include "dg_log.h"
#include "err.h"
#include "fp_as608.h"
#include "timeutil.h"
#include "uart_hal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- 链路:uart_hal 收字节进环形缓冲,持久解析器逐帧吐出 ---- */

#define PROBE_RING_SZ 4096
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static uint8_t s_ring[PROBE_RING_SZ];
static size_t s_ring_len;
static fp_parser_t s_parser;

static void on_rx(const uint8_t *d, size_t n, void *ud)
{
    (void)ud;
    pthread_mutex_lock(&s_mtx);
    if (n > sizeof(s_ring) - s_ring_len)
        n = sizeof(s_ring) - s_ring_len;
    memcpy(s_ring + s_ring_len, d, n);
    s_ring_len += n;
    pthread_mutex_unlock(&s_mtx);
}

static void ring_pull(size_t n)
{
    pthread_mutex_lock(&s_mtx);
    s_ring_len -= n;
    memmove(s_ring, s_ring + n, s_ring_len);
    pthread_mutex_unlock(&s_mtx);
}

static int wait_frame(fp_frame_t *out, int timeout_ms)
{
    int64_t deadline = now_mono_ms() + timeout_ms;
    for (;;) {
        fp_frame_t f;
        size_t consumed = 0;
        pthread_mutex_lock(&s_mtx);
        size_t len = s_ring_len;
        pthread_mutex_unlock(&s_mtx);
        if (len > 0) {
            int r = fp_as608_parse(&s_parser, s_ring, len, &f, &consumed);
            if (consumed)
                ring_pull(consumed);
            if (r == 1) {
                *out = f;
                return DG_OK;
            }
            continue;                     /* -1 已自动复位;0 需更多字节 */
        }
        if (now_mono_ms() >= deadline)
            return DG_ERR_TIMEOUT;
        /* 收字节由 uart_hal 回调线程灌进 s_ring,这里只等 */
        struct timespec ts = { 0, 3 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
}

/* 发帧 + 取一个应答帧;返回确认码(拿不到帧置 0xFFFF) */
static int xchg(const uint8_t *frame, size_t len, fp_frame_t *ack,
                uint16_t *confirm, int timeout_ms)
{
    if (uart_hal_send(frame, len) != DG_OK)
        return DG_ERR_IO;
    int rc = wait_frame(ack, timeout_ms);
    if (rc != DG_OK)
        return rc;
    *confirm = fp_as608_ack_confirm(ack);
    return DG_OK;
}

static const char *cname(uint16_t c)
{
    return fp_as608_confirm_name(c);
}

/* 打一行十六进制(排障要看原字节) */
static void dump_hex(const char *tag, const uint8_t *p, size_t n)
{
    printf("%s [%zuB]", tag, n);
    for (size_t i = 0; i < n; i++)
        printf(" %02X", p[i]);
    printf("\n");
}

/* ---- 子命令 ---- */

static int cmd_verify(void)
{
    uint8_t b[FP_A608_FRAME_MAX];
    fp_frame_t ack;
    uint16_t c = 0xFFFF;
    size_t n = fp_as608_verify_psw(b, sizeof(b), 0);
    int rc = xchg(b, n, &ack, &c, 2000);
    if (rc != DG_OK) {
        printf("verify: 无应答(rc=%d)\n", rc);
        return 1;
    }
    dump_hex("verify ack", ack.payload, ack.payload_len);
    printf("verify: confirm=0x%02X(%s)\n", c, cname(c));
    return c == FP_ACK_OK ? 0 : 1;
}

static int cmd_count(void)
{
    uint8_t b[FP_A608_FRAME_MAX];
    fp_frame_t ack;
    uint16_t c = 0xFFFF;
    size_t n = fp_as608_valid_num(b, sizeof(b));
    if (xchg(b, n, &ack, &c, 2000) != DG_OK) {
        printf("count: 无应答\n");
        return 1;
    }
    if (c != FP_ACK_OK) {
        printf("count: confirm=0x%02X(%s)\n", c, cname(c));
        return 1;
    }
    /* payload = [确认码 1B, 模板数 2B] */
    uint16_t cnt = (uint16_t)(((uint16_t)ack.payload[1] << 8) | ack.payload[2]);
    printf("count: 模组模板数 N=%u\n", cnt);
    return 0;
}

static int cmd_scan(int from, int to)
{
    uint8_t b[FP_A608_FRAME_MAX];
    fp_frame_t ack;
    uint16_t c;
    int occupied = 0, empty = 0;
    printf("scan: 探测 page %d..%d(LoadChar)\n", from, to);
    for (int p = from; p <= to; p++) {
        size_t n = fp_as608_load_char(b, sizeof(b), FP_A608_BUF2, (uint16_t)p);
        if (xchg(b, n, &ack, &c, 1000) != DG_OK) {
            printf("  page %d: 无应答\n", p);
            return 1;
        }
        if (c == FP_ACK_OK) {
            printf("  page %d: 有模板\n", p);
            occupied++;
        } else {
            /* 空页确认码:对账要靠它把"空页"与"读库错"分开,这里逐页打印原码 */
            printf("  page %d: 空(confirm=0x%02X=%s)\n", p, c, cname(c));
            empty++;
        }
    }
    printf("scan 汇总: 有模板 %d,空 %d\n", occupied, empty);
    return 0;
}

/* LoadChar(page) → UpChar(B2) → 取 512B 数据包;成功返回 0 并给 data/len */
static int read_template(uint16_t page, uint8_t *data, size_t cap, size_t *out_len,
                         int verbose)
{
    uint8_t b[FP_A608_FRAME_MAX];
    fp_frame_t ack;
    uint16_t c;

    size_t n = fp_as608_load_char(b, sizeof(b), FP_A608_BUF2, page);
    if (xchg(b, n, &ack, &c, 2000) != DG_OK) {
        printf("  LoadChar(%u): 无应答\n", page);
        return DG_ERR_IO;
    }
    if (c != FP_ACK_OK) {
        printf("  LoadChar(%u): confirm=0x%02X(%s)\n", page, c, cname(c));
        return DG_ERR_NOT_FOUND;
    }

    n = fp_as608_build_cmd(b, sizeof(b), 0x08, (const uint8_t[]){ FP_A608_BUF2 }, 1);
    if (xchg(b, n, &ack, &c, 2000) != DG_OK) {
        printf("  UpChar ACK: 无应答\n");
        return DG_ERR_IO;
    }
    if (verbose)
        dump_hex("  UpChar ACK", ack.payload, ack.payload_len);
    if (c != FP_ACK_OK) {
        printf("  UpChar: confirm=0x%02X(%s)\n", c, cname(c));
        return DG_ERR_IO;
    }

    /* 之后可能是:数据包(0x02)+结束包(0x08);也可能只有其一,容错扫描 */
    int got = 0;
    for (int i = 0; i < 4; i++) {
        fp_frame_t f;
        int rc = wait_frame(&f, got ? 800 : 3000);
        if (rc != DG_OK) {
            if (verbose)
                printf("  (第 %d 帧等待超时)\n", i + 1);
            break;
        }
        if (verbose)
            printf("  帧 %d: type=0x%02X payload_len=%u\n", i + 1, f.type, f.payload_len);
        if (f.type == FP_A608_TYPE_DATA) {
            if ((size_t)f.payload_len > cap) {
                printf("  数据包 %u 字节超出缓冲 %zu\n", f.payload_len, cap);
                return DG_ERR_NO_MEMORY;
            }
            memcpy(data, f.payload, f.payload_len);
            *out_len = f.payload_len;
            got = 1;
        } else if (f.type == FP_A608_TYPE_END) {
            break;                        /* 结束包:本次读模板完 */
        }
    }
    if (!got) {
        printf("  UpChar 未取得数据包(帧式可能不符)\n");
        return DG_ERR_IO;
    }
    return DG_OK;
}

static int cmd_dump(uint16_t page, const char *path)
{
    uint8_t data[FP_A608_PAYLOAD_MAX];
    size_t len = 0;
    if (read_template(page, data, sizeof(data), &len, 1) != DG_OK)
        return 1;
    printf("dump: page %u 读到 %zu 字节\n", page, len);
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("dump: 打不开 %s\n", path);
        return 1;
    }
    fwrite(data, 1, len, f);
    fclose(f);
    printf("dump: 已写 %s\n", path);
    return 0;
}

/* DownChar: 指令头 → 数据包 → 结束包,三步各有应答 */
static int write_template(uint16_t page, const uint8_t *data, size_t len)
{
    uint8_t b[FP_A608_FRAME_MAX];
    fp_frame_t ack;
    uint16_t c;

    /* 1. 指令头(0x09 + BufferID) */
    size_t n = fp_as608_down_char(b, sizeof(b), FP_A608_BUF2);
    if (xchg(b, n, &ack, &c, 2000) != DG_OK) {
        printf("  DownChar 指令头: 无应答\n");
        return DG_ERR_IO;
    }
    if (c != FP_ACK_OK) {
        printf("  DownChar 指令头: confirm=0x%02X(%s)\n", c, cname(c));
        return DG_ERR_IO;
    }

    /* 2. 数据包(0x02,512B) */
    n = fp_as608_build_data(b, sizeof(b), data, len);
    if (n == 0) {
        printf("  数据包组帧失败(len=%zu)\n", len);
        return DG_ERR_PARAM;
    }
    if (xchg(b, n, &ack, &c, 3000) != DG_OK) {
        printf("  DownChar 数据包: 无应答\n");
        return DG_ERR_IO;
    }
    if (c != FP_ACK_OK) {
        printf("  DownChar 数据包: confirm=0x%02X(%s)\n", c, cname(c));
        return DG_ERR_IO;
    }

    /* 3. 结束包(0x08) */
    n = fp_as608_build_end(b, sizeof(b));
    if (xchg(b, n, &ack, &c, 2000) != DG_OK) {
        printf("  DownChar 结束包: 无应答\n");
        return DG_ERR_IO;
    }
    if (c != FP_ACK_OK) {
        printf("  DownChar 结束包: confirm=0x%02X(%s)\n", c, cname(c));
        return DG_ERR_IO;
    }

    /* 4. Store 到目标页 */
    n = fp_as608_store(b, sizeof(b), FP_A608_BUF2, page);
    if (xchg(b, n, &ack, &c, 2000) != DG_OK) {
        printf("  Store: 无应答\n");
        return DG_ERR_IO;
    }
    if (c != FP_ACK_OK) {
        printf("  Store: confirm=0x%02X(%s)\n", c, cname(c));
        return DG_ERR_IO;
    }
    return DG_OK;
}

static int cmd_put(const char *path, uint16_t page)
{
    uint8_t want[FP_A608_PAYLOAD_MAX];
    size_t want_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("put: 打不开 %s\n", path);
        return 2;
    }
    want_len = fread(want, 1, sizeof(want), f);
    fclose(f);
    printf("put: 源 %s(%zu 字节)→ page %u\n", path, want_len, page);

    if (write_template(page, want, want_len) != DG_OK) {
        printf("put: 写入失败\n");
        return 1;
    }
    printf("put: DownChar+Store 成功,回读校验...\n");

    uint8_t got[FP_A608_PAYLOAD_MAX];
    size_t got_len = 0;
    if (read_template(page, got, sizeof(got), &got_len, 1) != DG_OK) {
        printf("put: 回读失败(写入可能未生效)\n");
        return 1;
    }
    if (got_len != want_len) {
        printf("put: 长度不一致 源 %zu / 回读 %zu\n", want_len, got_len);
        return 3;
    }
    if (memcmp(want, got, want_len) != 0) {
        size_t i = 0;
        while (i < want_len && want[i] == got[i])
            i++;
        printf("put: 内容不一致(首个差异 @%zu: 源 %02X / 回读 %02X)\n",
               i, want[i], got[i]);
        return 3;
    }
    printf("put: **往返一致**(%zu 字节) —— DownChar 可用\n", want_len);
    return 0;
}

static int cmd_del(uint16_t page, uint16_t count)
{
    uint8_t b[FP_A608_FRAME_MAX];
    fp_frame_t ack;
    uint16_t c = 0xFFFF;
    size_t n = fp_as608_delet_char(b, sizeof(b), page, count);
    if (xchg(b, n, &ack, &c, 2000) != DG_OK) {
        printf("del: 无应答\n");
        return 1;
    }
    printf("del: page %u count %u → confirm=0x%02X(%s)\n", page, count, c, cname(c));
    return c == FP_ACK_OK ? 0 : 1;
}

static int cmd_empty(void)
{
    uint8_t b[FP_A608_FRAME_MAX];
    fp_frame_t ack;
    uint16_t c = 0xFFFF;
    size_t n = fp_as608_empty(b, sizeof(b));
    if (xchg(b, n, &ack, &c, 5000) != DG_OK) {
        printf("empty: 无应答\n");
        return 1;
    }
    printf("empty: confirm=0x%02X(%s)\n", c, cname(c));
    return c == FP_ACK_OK ? 0 : 1;
}

/* raw <hex> [wait_ms] — 原字节诊断:发任意字节,然后把 wait_ms 内收到的
 * 每一帧打出来(类型/长度/前若干字节)。用于 UpChar 这类"应答 OK 但不吐
 * 数据包"的悬案:换缓冲区号、换指令码、拉长等待都靠它,不必改代码重编。 */
static int cmd_raw(const char *hex, int wait_ms)
{
    uint8_t buf[FP_A608_FRAME_MAX];
    size_t n = 0;
    for (const char *p = hex; *p && n < sizeof(buf); ) {
        while (*p == ' ' || *p == ':')
            p++;
        if (!*p)
            break;
        char two[3] = { p[0], p[1] ? p[1] : '0', 0 };
        if (p[1])
            p += 2;
        else
            p += 1;
        buf[n++] = (uint8_t)strtoul(two, NULL, 16);
    }
    if (!n) {
        printf("raw: 没有可发字节\n");
        return 2;
    }
    dump_hex("raw 发送", buf, n);
    if (uart_hal_send(buf, n) != DG_OK) {
        printf("raw: 发送失败\n");
        return 1;
    }
    int64_t deadline = now_mono_ms() + wait_ms;
    int frames = 0;
    while (now_mono_ms() < deadline) {
        fp_frame_t f;
        if (wait_frame(&f, (int)(deadline - now_mono_ms())) != DG_OK)
            break;
        frames++;
        printf("raw 收帧 %d: type=0x%02X payload_len=%u", frames, f.type, f.payload_len);
        for (int k = 0; k < f.payload_len && k < 8; k++)
            printf(" %02X", f.payload[k]);
        if (f.payload_len > 8)
            printf(" ...");
        printf("\n");
    }
    printf("raw: 共收到 %d 帧(%d ms 窗口)\n", frames, wait_ms);
    return frames ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/ttyS8";
    int baud = 57600;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc)
            dev = argv[++i];
        else if (!strcmp(argv[i], "-b") && i + 1 < argc)
            baud = atoi(argv[++i]);
        else {
            printf("未知选项 %s\n", argv[i]);
            return 2;
        }
    }
    if (i >= argc) {
        printf("用法: fp_probe [-d /dev/ttyS8] [-b 57600] <verify|count|scan|dump|put|del|empty|raw>\n");
        return 2;
    }
    const char *cmd = argv[i++];

    uart_config_t cfg = { .device = dev, .baud = baud, .data_bits = 8,
                          .parity = 'N', .stop_bits = 1 };
    fp_as608_parser_init(&s_parser);
    if (uart_hal_open(&cfg, on_rx, NULL) != DG_OK) {
        printf("打开 %s 失败(应用可能占用中:先 S60doorguard stop)\n", dev);
        return 1;
    }
    usleep(100 * 1000);                   /* 模组上电稳定期 */

    int rc = 0;
    if (!strcmp(cmd, "verify")) {
        rc = cmd_verify();
    } else if (!strcmp(cmd, "count")) {
        rc = cmd_count();
    } else if (!strcmp(cmd, "scan")) {
        int from = (i < argc) ? atoi(argv[i]) : 0;
        int to = (i + 1 < argc) ? atoi(argv[i + 1]) : 999;
        if (from < 0 || to > 999 || from > to) {
            printf("scan 页范围非法\n");
            rc = 2;
        } else {
            rc = cmd_scan(from, to);
        }
    } else if (!strcmp(cmd, "dump") && i + 1 < argc) {
        rc = cmd_dump((uint16_t)atoi(argv[i]), argv[i + 1]);
    } else if (!strcmp(cmd, "put") && i + 1 < argc) {
        rc = cmd_put(argv[i], (uint16_t)atoi(argv[i + 1]));
    } else if (!strcmp(cmd, "del") && i < argc) {
        rc = cmd_del((uint16_t)atoi(argv[i]), (i + 1 < argc) ? (uint16_t)atoi(argv[i + 1]) : 1);
    } else if (!strcmp(cmd, "empty")) {
        rc = cmd_empty();
    } else if (!strcmp(cmd, "raw") && i < argc) {
        rc = cmd_raw(argv[i], (i + 1 < argc) ? atoi(argv[i + 1]) : 3000);
    } else {
        printf("未知/缺参子命令 %s\n", cmd);
        rc = 2;
    }

    uart_hal_close();
    return rc;
}
