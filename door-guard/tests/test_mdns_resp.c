/*
 * test_mdns_resp.c — mDNS 应答器 socket 层回环测试(B8-5,2026-10-25)
 *
 * 起真应答器(mdns_start,绑 5353;WSL root 可跑),经 UDP 单播发 DNS 查询:
 *   自己的名字(dgtest.local A)→ 有应答,且应答含主机名;
 *   别人的名字(nope.local)→ 静默(超时无应答)。
 * wire 层编解码已有 test_mdns_wire,本件只测「查询进来 → 应答出去」的链路。
 */
#include "dg_test.h"
#include "mdns_responder.h"
#include "storage.h"
#include "cfg.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* 组 DNS 查询包:header(12B) + QNAME(压缩名) + QTYPE/QCLASS(4B) */
static int build_query(uint8_t *out, size_t cap, const char *name_no_local)
{
    if (cap < 12 + strlen(name_no_local) + 8)
        return -1;
    memset(out, 0, 12);                      /* ID=0 flags=0 QDCOUNT=1 */
    out[4] = 0;
    out[5] = 1;
    size_t o = 12;
    const char *p = name_no_local;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        out[o++] = (uint8_t)len;
        memcpy(out + o, p, len);
        o += len;
        p += len;
        if (dot)
            p++;                             /* 跳过 '.' */
    }
    out[o++] = 0;                            /* 根 */
    out[o++] = 0; out[o++] = 1;              /* QTYPE=A */
    out[o++] = 0; out[o++] = 1;              /* QCLASS=IN */
    return (int)o;
}

/* 发查询并等应答;got_reply 回传是否在 timeout_ms 内收到 */
static void probe(const char *name, bool *got_reply, char rbuf[1500], ssize_t *rlen)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    DG_CHECK(s >= 0);
    struct timeval tv = { 2, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t q[512];
    int qlen = build_query(q, sizeof(q), name);
    DG_CHECK(qlen > 0);
    struct sockaddr_in dst = { 0 };
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = inet_addr("127.0.0.1");
    dst.sin_port = htons(5353);
    DG_CHECK(sendto(s, q, (size_t)qlen, 0, (struct sockaddr *)&dst, sizeof(dst)) == qlen);

    *rlen = recvfrom(s, rbuf, 1500, 0, NULL, NULL);
    *got_reply = (*rlen > 0);
    close(s);
}

int main(void)
{
    /* 沙箱 cfg:主机名 dgtest(应答器启动时读 device_config) */
    char dir[64], db[128], key[128];
    snprintf(dir, sizeof(dir), "/tmp/dg_mdns_%d", (int)getpid());
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", dir, dir);
    if (system(cmd) != 0)
        exit(1);
    snprintf(db, sizeof(db), "%s/db.sqlite", dir);
    snprintf(key, sizeof(key), "%s/dg.key", dir);
    DG_CHECK(storage_init(db, key) == DG_OK);
    DG_CHECK(db_config_set("mdns_host", "dgtest") == DG_OK);
    DG_CHECK(db_config_set("web_port", "8080") == DG_OK);

    /* cfg 内存态同样给出(mdns_start 两处读谁以实现为准,双保险) */
    DG_CHECK(cfg_load(NULL, NULL) == DG_OK);
    DG_CHECK(cfg_set_str("network.web_port", "8080") == DG_OK);

    int rc = mdns_start();
    if (rc != DG_OK) {
        /* 5353 被占/组播入组失败的环境(非 root CI):SKIP 不算失败 */
        printf("[MDNS] mdns_start 失败(%d):socket 层用例 SKIP(需 root + 组播)\n", rc);
        storage_deinit();
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
        (void)system(cmd);
        DG_TEST_EXIT();
    }
    DG_CHECK(mdns_running());

    /* 自己的名字 → 有应答且含主机名 */
    bool got = false;
    char rbuf[1500] = { 0 };
    ssize_t rlen = 0;
    probe("dgtest.local", &got, rbuf, &rlen);
    DG_CHECK(got);
    DG_CHECK(memmem(rbuf, (size_t)rlen, "dgtest", 6) != NULL);

    /* 别人的名字 → 静默(2s 超时无应答) */
    got = true;
    probe("nope123.local", &got, rbuf, &rlen);
    DG_CHECK(!got);

    mdns_stop();
    DG_CHECK(!mdns_running());
    storage_deinit();
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    (void)system(cmd);
    DG_TEST_EXIT();
}
