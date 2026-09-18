/*
 * test_mdns_wire.c — mDNS 报文编解码(mdns_wire.h)
 *
 * 为什么值得单独测:报文是字节级协议,写错只能靠抓包发现;这里对着
 * RFC 1035 §4.1 的布局逐字节断言,把"名字编解码/记录写入/解析"钉死。
 * 覆盖:名字编码边界(空标签/超长标签/根)、名字解码压缩指针与防环、
 * 名字大小写比较、问题段解析(QU 位/多问题)、RR 遍历、记录写入自洽性。
 */
#include "dg_test.h"
#include "mdns_wire.h"

#include <arpa/inet.h>
#include <string.h>

static void t_name_encode(void)
{
    printf("[MW1] 名字编码:点分名 → <len>label 序列 + 0x00\n");
    uint8_t buf[128];
    int n = mdns_name_encode("doorguard.local", buf, sizeof(buf));
    DG_CHECK(n == 17);
    DG_CHECK(buf[0] == 9 && memcmp(buf + 1, "doorguard", 9) == 0);
    DG_CHECK(buf[10] == 5 && memcmp(buf + 11, "local", 5) == 0);
    DG_CHECK(buf[16] == 0x00);

    n = mdns_name_encode("_http._tcp.local", buf, sizeof(buf));
    DG_CHECK(n == 18);
    DG_CHECK(buf[0] == 5 && memcmp(buf + 1, "_http", 5) == 0);
    DG_CHECK(buf[6] == 4 && memcmp(buf + 7, "_tcp", 4) == 0);

    n = mdns_name_encode(".", buf, sizeof(buf));      /* 根 */
    DG_CHECK(n == 1 && buf[0] == 0);

    DG_CHECK(mdns_name_encode(NULL, buf, sizeof(buf)) == DG_ERR_PARAM);
    DG_CHECK(mdns_name_encode("a..b", buf, sizeof(buf)) == DG_ERR_PARAM);
    DG_CHECK(mdns_name_encode("", buf, sizeof(buf)) == DG_ERR_PARAM);

    char big[80];                                    /* 标签超 63 字节:非法 */
    memset(big, 'x', sizeof(big));
    big[64] = '.';
    big[65] = 'a';
    big[66] = '\0';
    DG_CHECK(mdns_name_encode(big, buf, sizeof(buf)) == DG_ERR_PARAM);
    DG_CHECK(mdns_name_encode("doorguard.local", buf, 4) == DG_ERR_NO_MEMORY);
}

static void t_name_decode(void)
{
    printf("[MW2] 名字解码:普通序列 + 压缩指针 + 防环/越界\n");
    uint8_t pkt[128];
    int n = mdns_name_encode("doorguard.local", pkt, sizeof(pkt));
    DG_CHECK(n > 0);
    char out[MDNS_MAX_NAME];
    DG_CHECK(mdns_name_decode(pkt, (size_t)n, 0, out, sizeof(out)) == n);
    DG_CHECK(strcmp(out, "doorguard.local") == 0);

    /* 压缩指针:名 "a" 后面跟一个指向偏移 0 的指针 */
    uint8_t pkt2[128];
    int off = mdns_name_encode("doorguard.local", pkt2, sizeof(pkt2));
    pkt2[off] = 1;
    pkt2[off + 1] = 'a';
    pkt2[off + 2] = 0xC0;
    pkt2[off + 3] = 0x00;
    int used = mdns_name_decode(pkt2, (size_t)off + 4, (size_t)off, out,
                                sizeof(out));
    DG_CHECK(used == 4);
    DG_CHECK(strcmp(out, "a.doorguard.local") == 0);

    uint8_t pkt3[4] = { 0xC0, 0x00, 0x00, 0x00 };    /* 自指环:必须拒绝 */
    DG_CHECK(mdns_name_decode(pkt3, sizeof(pkt3), 0, out, sizeof(out)) < 0);
    uint8_t pkt4[6] = { 0xC0, 0x04, 0, 0, 1, 'a' };  /* 前向引用:拒绝 */
    DG_CHECK(mdns_name_decode(pkt4, sizeof(pkt4), 0, out, sizeof(out)) < 0);
    uint8_t pkt5[2] = { 9, 'x' };                    /* 越界 */
    DG_CHECK(mdns_name_decode(pkt5, sizeof(pkt5), 0, out, sizeof(out)) < 0);
}

static void t_name_equal(void)
{
    printf("[MW3] 名字比较:大小写不敏感、忽略末尾点\n");
    DG_CHECK(mdns_name_equal("doorguard.local", "doorguard.local"));
    DG_CHECK(mdns_name_equal("DoorGuard.LOCAL", "doorguard.local"));
    DG_CHECK(mdns_name_equal("doorguard.local.", "doorguard.local"));
    DG_CHECK(!mdns_name_equal("doorguard.local", "doorguard2.local"));
    DG_CHECK(!mdns_name_equal("doorguard.local", "other.local"));
    DG_CHECK(!mdns_name_equal(NULL, "a"));
    DG_CHECK(!mdns_name_equal("a", "ab"));
}

static void t_parse_query(void)
{
    printf("[MW4] 查询解析:ID/flags/QU 位/多问题/坏报文\n");
    uint8_t buf[256];
    mdns_msg_t m;
    mdns_msg_begin(&m, buf, sizeof(buf), 0x1234, 0);
    mdns_msg_put_question(&m, "doorguard.local", MDNS_TYPE_A,
                          (uint16_t)(MDNS_CLASS_IN | MDNS_CLASS_TOP)); /* QU=1 */
    mdns_msg_put_question(&m, "_http._tcp.local", MDNS_TYPE_PTR, MDNS_CLASS_IN);
    DG_CHECK(mdns_msg_end(&m) == DG_OK);

    uint16_t id = 0, flags = 0xFFFF;
    mdns_question_t qs[4];
    int nq = mdns_parse_query(buf, m.len, &id, &flags, qs, 4);
    DG_CHECK(nq == 2);
    DG_CHECK(id == 0x1234);
    DG_CHECK(flags == 0);
    DG_CHECK(strcmp(qs[0].name, "doorguard.local") == 0);
    DG_CHECK(qs[0].type == MDNS_TYPE_A);
    DG_CHECK(qs[0].klass == MDNS_CLASS_IN);
    DG_CHECK(qs[0].unicast == true);                 /* QU 位识别并剥离 */
    DG_CHECK(strcmp(qs[1].name, "_http._tcp.local") == 0);
    DG_CHECK(qs[1].type == MDNS_TYPE_PTR);
    DG_CHECK(qs[1].unicast == false);
    DG_CHECK(buf[4] == 0 && buf[5] == 2);            /* 头计数与实写一致 */

    DG_CHECK(mdns_parse_query(buf, 4, &id, &flags, qs, 4) < 0);
    DG_CHECK(mdns_parse_query(NULL, 0, &id, &flags, qs, 4) < 0);
}

typedef struct {
    int      a, ptr, srv, txt;
    uint16_t a_class;
    uint32_t a_ttl;
    uint8_t  a_ip[4];
    uint16_t srv_port;
    uint16_t srv_class;
} rr_seen_t;

static int count_rr(const mdns_rr_t *rr, void *ud)
{
    rr_seen_t *s = (rr_seen_t *)ud;
    if (rr->type == MDNS_TYPE_A && rr->rdlen == 4) {
        s->a++;
        s->a_class = rr->klass;
        s->a_ttl = rr->ttl;
        memcpy(s->a_ip, rr->rdata, 4);
    } else if (rr->type == MDNS_TYPE_PTR) {
        s->ptr++;
    } else if (rr->type == MDNS_TYPE_SRV && rr->rdlen >= 6) {
        s->srv++;
        s->srv_class = rr->klass;
        s->srv_port = (uint16_t)((rr->rdata[4] << 8) | rr->rdata[5]);
    } else if (rr->type == MDNS_TYPE_TXT) {
        s->txt++;
    }
    return 0;
}

static void t_records(void)
{
    printf("[MW5] 记录写入:A 的类/TTL/RDLENGTH/RDATA 与遍历自洽\n");
    uint8_t buf[512];
    mdns_msg_t m;
    uint8_t ip[4] = { 192, 168, 2, 95 };
    static const char *const txt[] = { "txtvers=1", "path=/" };

    mdns_msg_begin(&m, buf, sizeof(buf), 0,
                   (uint16_t)(MDNS_FLAG_RESPONSE | MDNS_FLAG_AA));
    DG_CHECK(mdns_msg_put_a(&m, "doorguard.local", 120, true, ip) == DG_OK);
    DG_CHECK(mdns_msg_put_ptr(&m, "_http._tcp.local", 4500,
                              "doorguard._http._tcp.local") == DG_OK);
    DG_CHECK(mdns_msg_put_srv(&m, "doorguard._http._tcp.local", 4500, true, 0,
                              0, 8080, "doorguard.local") == DG_OK);
    DG_CHECK(mdns_msg_put_txt(&m, "doorguard._http._tcp.local", 4500, true,
                              txt, 2) == DG_OK);
    DG_CHECK(mdns_msg_put_additional_a(&m, "doorguard.local", 120, ip) == DG_OK);
    DG_CHECK(mdns_msg_end(&m) == DG_OK);

    DG_CHECK(buf[2] == 0x84 && buf[3] == 0x00);      /* QR=1 AA=1 */
    DG_CHECK(buf[6] == 0 && buf[7] == 4);            /* ANCOUNT=4 */
    DG_CHECK(buf[10] == 0 && buf[11] == 1);          /* ARCOUNT=1(附加 A) */

    /* A 记录逐字节:名字 17B + TYPE/CLASS/TTL/RDLENGTH */
    size_t o = 12 + 17;
    DG_CHECK(buf[o] == 0x00 && buf[o + 1] == 0x01);              /* TYPE=A */
    DG_CHECK(buf[o + 2] == 0x80 && buf[o + 3] == 0x01);          /* IN|flush */
    DG_CHECK(buf[o + 4] == 0 && buf[o + 5] == 0 && buf[o + 6] == 0 &&
             buf[o + 7] == 120);                                 /* TTL=120 */
    DG_CHECK(buf[o + 8] == 0x00 && buf[o + 9] == 0x04);          /* RDLEN=4 */
    DG_CHECK(memcmp(buf + o + 10, ip, 4) == 0);

    rr_seen_t seen;
    memset(&seen, 0, sizeof(seen));
    DG_CHECK(mdns_walk_rrs(buf, m.len, count_rr, &seen) == 0);
    DG_CHECK(seen.a == 2);                           /* 答案 1 + 附加 1 */
    DG_CHECK(seen.ptr == 1 && seen.srv == 1 && seen.txt == 1);
    DG_CHECK(seen.a_class == (MDNS_CLASS_IN | MDNS_CLASS_TOP));
    DG_CHECK(seen.a_ttl == 120);
    DG_CHECK(memcmp(seen.a_ip, ip, 4) == 0);
    DG_CHECK(seen.srv_port == 8080);

    /* goodbye 形态:TTL=0 的 A 记录 —— 局域网据此删缓存 */
    mdns_msg_t g;
    mdns_msg_begin(&g, buf, sizeof(buf), 0, 0);
    DG_CHECK(mdns_msg_put_a(&g, "doorguard.local", 0, true, ip) == DG_OK);
    DG_CHECK(mdns_msg_end(&g) == DG_OK);
    memset(&seen, 0, sizeof(seen));
    DG_CHECK(mdns_walk_rrs(buf, g.len, count_rr, &seen) == 0);
    DG_CHECK(seen.a == 1 && seen.a_ttl == 0);

    /* 缓冲不足:报错且不越界(小缓冲写大名字) */
    uint8_t small[24];
    mdns_msg_t m2;
    mdns_msg_begin(&m2, small, sizeof(small), 0, 0);
    DG_CHECK(mdns_msg_put_a(&m2, "doorguard.local", 120, true, ip) ==
             DG_ERR_NO_MEMORY);
    DG_CHECK(mdns_msg_end(&m2) == DG_ERR_NO_MEMORY);
}

int main(void)
{
    t_name_encode();
    t_name_decode();
    t_name_equal();
    t_parse_query();
    t_records();
    DG_TEST_EXIT();
}
