/*
 * mdns_wire.c — mDNS 报文编解码实现
 *
 * 布局(RFC 1035 §4,大端):
 *   头 12B:ID | FLAGS | QDCOUNT | ANCOUNT | NSCOUNT | ARCOUNT
 *   问题  :NAME | TYPE | CLASS
 *   记录  :NAME | TYPE | CLASS | TTL(4) | RDLENGTH | RDATA
 * NAME  :<len><label>... 0x00;<=0xC0 的两字节是压缩指针(仅解析侧处理)
 */
#include "mdns_wire.h"

#include <stdio.h>
#include <string.h>

/* ---- 写入器 ---- */

static int wr_u8(mdns_msg_t *m, uint8_t v)
{
    if (m->overflow)
        return DG_ERR_NO_MEMORY;
    if (m->len + 1 > m->cap) {
        m->overflow = true;
        return DG_ERR_NO_MEMORY;
    }
    m->buf[m->len++] = v;
    return DG_OK;
}

static int wr_u16(mdns_msg_t *m, uint16_t v)
{
    if (wr_u8(m, (uint8_t)(v >> 8)) != DG_OK)
        return DG_ERR_NO_MEMORY;
    return wr_u8(m, (uint8_t)(v & 0xFF));
}

static int wr_u32(mdns_msg_t *m, uint32_t v)
{
    if (wr_u16(m, (uint16_t)(v >> 16)) != DG_OK)
        return DG_ERR_NO_MEMORY;
    return wr_u16(m, (uint16_t)(v & 0xFFFF));
}

static int wr_bytes(mdns_msg_t *m, const void *p, size_t n)
{
    if (m->overflow)
        return DG_ERR_NO_MEMORY;
    if (m->len + n > m->cap) {
        m->overflow = true;
        return DG_ERR_NO_MEMORY;
    }
    memcpy(m->buf + m->len, p, n);
    m->len += n;
    return DG_OK;
}

int mdns_name_encode(const char *name, uint8_t *out, size_t cap)
{
    if (!name || !out)
        return DG_ERR_PARAM;
    if (name[0] == '\0')
        return DG_ERR_PARAM;                 /* 空名是调用方 bug,不是"根" */
    if (strcmp(name, ".") == 0) {
        if (cap < 1)
            return DG_ERR_NO_MEMORY;
        out[0] = 0x00;                       /* 根 */
        return 1;
    }

    size_t o = 0;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t lab = dot ? (size_t)(dot - p) : strlen(p);
        if (lab == 0) {                      /* "a..b" 空标签:非法 */
            return DG_ERR_PARAM;
        }
        if (lab > 63)
            return DG_ERR_PARAM;
        if (o + 1 + lab + 1 > cap)
            return DG_ERR_NO_MEMORY;
        out[o++] = (uint8_t)lab;
        memcpy(out + o, p, lab);
        o += lab;
        p = dot ? dot + 1 : p + lab;
    }
    if (o + 1 > cap)
        return DG_ERR_NO_MEMORY;
    out[o++] = 0x00;
    return (int)o;
}

static int wr_name(mdns_msg_t *m, const char *name)
{
    uint8_t tmp[MDNS_MAX_NAME];
    int n = mdns_name_encode(name, tmp, sizeof(tmp));
    if (n < 0)
        return n;
    return wr_bytes(m, tmp, (size_t)n);
}

void mdns_msg_begin(mdns_msg_t *m, uint8_t *buf, size_t cap, uint16_t id, uint16_t flags)
{
    memset(m, 0, sizeof(*m));
    m->buf = buf;
    m->cap = cap;
    m->len = 0;
    /* 头先占位;计数在 end 回填——省得调用方预先算记录条数 */
    if (cap < 12) {
        m->overflow = true;
        return;
    }
    wr_u16(m, id);
    wr_u16(m, flags);
    wr_u16(m, 0);
    wr_u16(m, 0);
    wr_u16(m, 0);
    wr_u16(m, 0);
}

int mdns_msg_end(mdns_msg_t *m)
{
    if (m->overflow)
        return DG_ERR_NO_MEMORY;
    m->buf[4] = (uint8_t)(m->qd >> 8);
    m->buf[5] = (uint8_t)(m->qd & 0xFF);
    m->buf[6] = (uint8_t)(m->an >> 8);
    m->buf[7] = (uint8_t)(m->an & 0xFF);
    m->buf[8] = (uint8_t)(m->ns >> 8);
    m->buf[9] = (uint8_t)(m->ns & 0xFF);
    m->buf[10] = (uint8_t)(m->ar >> 8);
    m->buf[11] = (uint8_t)(m->ar & 0xFF);
    return DG_OK;
}

int mdns_msg_put_question(mdns_msg_t *m, const char *name, uint16_t type, uint16_t klass)
{
    if (wr_name(m, name) < 0 || wr_u16(m, type) != DG_OK ||
        wr_u16(m, klass) != DG_OK)
        return DG_ERR_NO_MEMORY;
    m->qd++;
    return DG_OK;
}

/* 记录公共头(NAME/TYPE/CLASS/TTL/RDLENGTH 的 RDLENGTH 由调用方回填) */
static int rr_begin(mdns_msg_t *m, const char *name, uint16_t type, uint16_t klass,
                    uint32_t ttl, size_t *rdlen_off, bool is_additional)
{
    if (wr_name(m, name) < 0)
        return DG_ERR_NO_MEMORY;
    if (wr_u16(m, type) != DG_OK || wr_u16(m, klass) != DG_OK ||
        wr_u32(m, ttl) != DG_OK)
        return DG_ERR_NO_MEMORY;
    *rdlen_off = m->len;
    if (wr_u16(m, 0) != DG_OK)
        return DG_ERR_NO_MEMORY;
    if (is_additional)
        m->ar++;
    else
        m->an++;
    return DG_OK;
}

static int rr_end(mdns_msg_t *m, size_t rdlen_off, size_t rdlen)
{
    if (m->overflow || rdlen > 0xFFFF)
        return DG_ERR_NO_MEMORY;
    m->buf[rdlen_off] = (uint8_t)(rdlen >> 8);
    m->buf[rdlen_off + 1] = (uint8_t)(rdlen & 0xFF);
    return DG_OK;
}

/* 追加记录(additional=true 计数进 ARCOUNT —— SRV 的 target A 记录走这里) */
static int put_a_ex(mdns_msg_t *m, const char *name, uint32_t ttl, bool flush,
                    const uint8_t ip[4], bool additional)
{
    size_t off;
    if (rr_begin(m, name, MDNS_TYPE_A,
                 (uint16_t)(MDNS_CLASS_IN | (flush ? MDNS_CLASS_TOP : 0)),
                 ttl, &off, additional) != DG_OK)
        return DG_ERR_NO_MEMORY;
    if (wr_bytes(m, ip, 4) != DG_OK)
        return DG_ERR_NO_MEMORY;
    return rr_end(m, off, 4);
}

int mdns_msg_put_a(mdns_msg_t *m, const char *name, uint32_t ttl, bool flush,
                   const uint8_t ip[4])
{
    return put_a_ex(m, name, ttl, flush, ip, false);
}

int mdns_msg_put_ptr(mdns_msg_t *m, const char *name, uint32_t ttl, const char *target)
{
    size_t off, start;
    /* PTR 是共享记录:不带 cache-flush 位 */
    if (rr_begin(m, name, MDNS_TYPE_PTR, MDNS_CLASS_IN, ttl, &off, false) != DG_OK)
        return DG_ERR_NO_MEMORY;
    start = m->len;
    if (wr_name(m, target) < 0)
        return DG_ERR_NO_MEMORY;
    return rr_end(m, off, m->len - start);
}

int mdns_msg_put_srv(mdns_msg_t *m, const char *name, uint32_t ttl, bool flush,
                     uint16_t prio, uint16_t weight, uint16_t port, const char *target)
{
    size_t off, start;
    if (rr_begin(m, name, MDNS_TYPE_SRV,
                 (uint16_t)(MDNS_CLASS_IN | (flush ? MDNS_CLASS_TOP : 0)),
                 ttl, &off, false) != DG_OK)
        return DG_ERR_NO_MEMORY;
    start = m->len;
    if (wr_u16(m, prio) != DG_OK || wr_u16(m, weight) != DG_OK ||
        wr_u16(m, port) != DG_OK)
        return DG_ERR_NO_MEMORY;
    if (wr_name(m, target) < 0)
        return DG_ERR_NO_MEMORY;
    return rr_end(m, off, m->len - start);
}

int mdns_msg_put_txt(mdns_msg_t *m, const char *name, uint32_t ttl, bool flush,
                     const char *const *kv, int nkv)
{
    size_t off, start;
    if (rr_begin(m, name, MDNS_TYPE_TXT,
                 (uint16_t)(MDNS_CLASS_IN | (flush ? MDNS_CLASS_TOP : 0)),
                 ttl, &off, false) != DG_OK)
        return DG_ERR_NO_MEMORY;
    start = m->len;
    for (int i = 0; i < nkv; i++) {
        size_t l = kv[i] ? strlen(kv[i]) : 0;
        if (l > 255) {
            m->overflow = true;
            return DG_ERR_NO_MEMORY;
        }
        if (wr_u8(m, (uint8_t)l) != DG_OK ||
            wr_bytes(m, kv[i], l) != DG_OK)
            return DG_ERR_NO_MEMORY;
    }
    return rr_end(m, off, m->len - start);
}

/* SRV 的附加 A 记录(additional 段;mDNS 客户端拿到 SRV 后需要 target 地址) */
int mdns_msg_put_additional_a(mdns_msg_t *m, const char *name, uint32_t ttl,
                              const uint8_t ip[4])
{
    return put_a_ex(m, name, ttl, true, ip, true);
}

/* ---- 解析 ---- */

int mdns_name_decode(const uint8_t *pkt, size_t pkt_len, size_t off,
                     char *out, size_t cap)
{
    if (!pkt || !out || cap == 0)
        return DG_ERR_PARAM;
    size_t o = 0, i = off;
    bool jumped = false;
    int consumed = 0;
    int hops = 0;

    out[0] = '\0';
    for (;;) {
        if (i >= pkt_len)
            return DG_ERR_PARAM;
        uint8_t len = pkt[i];
        if (len == 0) {
            if (!jumped)
                consumed = (int)(i - off) + 1;
            break;
        }
        if ((len & 0xC0) == 0xC0) {          /* 压缩指针 */
            if (i + 1 >= pkt_len)
                return DG_ERR_PARAM;
            size_t target = (size_t)(((len & 0x3F) << 8) | pkt[i + 1]);
            if (!jumped)
                consumed = (int)(i - off) + 2;
            jumped = true;
            if (target >= pkt_len || target >= i)
                return DG_ERR_PARAM;         /* 只允许向前引用,防环 */
            i = target;
            if (++hops > 16)
                return DG_ERR_PARAM;
            continue;
        }
        if ((len & 0xC0) != 0)
            return DG_ERR_PARAM;             /* 0x40/0x80 保留位 */
        if (i + 1 + len > pkt_len)
            return DG_ERR_PARAM;
        if (o + len + 2 > cap)
            return DG_ERR_NO_MEMORY;
        if (o)
            out[o++] = '.';
        memcpy(out + o, pkt + i + 1, len);
        o += len;
        out[o] = '\0';
        i += 1 + len;
        if (!jumped)
            consumed = (int)(i - off);
    }
    return consumed;
}

bool mdns_name_equal(const char *a, const char *b)
{
    if (!a || !b)
        return false;
    size_t la = strlen(a), lb = strlen(b);
    while (la > 0 && a[la - 1] == '.')
        la--;
    while (lb > 0 && b[lb - 1] == '.')
        lb--;
    if (la != lb)
        return false;
    for (size_t i = 0; i < la; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return false;
    }
    return true;
}

int mdns_parse_query(const uint8_t *pkt, size_t pkt_len, uint16_t *id,
                     uint16_t *flags, mdns_question_t *qs, int max_q)
{
    if (!pkt || pkt_len < 12)
        return DG_ERR_PARAM;
    uint16_t f = (uint16_t)((pkt[2] << 8) | pkt[3]);
    uint16_t qd = (uint16_t)((pkt[4] << 8) | pkt[5]);
    if (id)
        *id = (uint16_t)((pkt[0] << 8) | pkt[1]);
    if (flags)
        *flags = f;

    size_t off = 12;
    int n = 0;
    for (uint16_t i = 0; i < qd; i++) {
        char name[MDNS_MAX_NAME];
        int used = mdns_name_decode(pkt, pkt_len, off, name, sizeof(name));
        if (used < 0)
            return DG_ERR_PARAM;
        off += (size_t)used;
        if (off + 4 > pkt_len)
            return DG_ERR_PARAM;
        uint16_t type = (uint16_t)((pkt[off] << 8) | pkt[off + 1]);
        uint16_t klass = (uint16_t)((pkt[off + 2] << 8) | pkt[off + 3]);
        off += 4;
        if (qs && n < max_q) {
            snprintf(qs[n].name, sizeof(qs[n].name), "%s", name);
            qs[n].type = type;
            qs[n].unicast = (klass & MDNS_CLASS_TOP) != 0;
            qs[n].klass = (uint16_t)(klass & MDNS_CLASS_MASK);
        }
        n++;
    }
    return n;
}

int mdns_walk_rrs(const uint8_t *pkt, size_t pkt_len,
                  int (*cb)(const mdns_rr_t *rr, void *ud), void *ud)
{
    if (!pkt || pkt_len < 12)
        return DG_ERR_PARAM;
    uint16_t qd = (uint16_t)((pkt[4] << 8) | pkt[5]);
    uint16_t an = (uint16_t)((pkt[6] << 8) | pkt[7]);
    uint16_t ns = (uint16_t)((pkt[8] << 8) | pkt[9]);
    uint16_t ar = (uint16_t)((pkt[10] << 8) | pkt[11]);

    size_t off = 12;
    for (uint16_t i = 0; i < qd; i++) {      /* 先跳过问题段 */
        char name[MDNS_MAX_NAME];
        int used = mdns_name_decode(pkt, pkt_len, off, name, sizeof(name));
        if (used < 0)
            return DG_ERR_PARAM;
        off += (size_t)used + 4;
        if (off > pkt_len)
            return DG_ERR_PARAM;
    }

    uint32_t total = (uint32_t)an + ns + ar;
    for (uint32_t i = 0; i < total; i++) {
        mdns_rr_t rr;
        int used = mdns_name_decode(pkt, pkt_len, off, rr.name, sizeof(rr.name));
        if (used < 0)
            return DG_ERR_PARAM;
        off += (size_t)used;
        if (off + 10 > pkt_len)
            return DG_ERR_PARAM;
        rr.type = (uint16_t)((pkt[off] << 8) | pkt[off + 1]);
        rr.klass = (uint16_t)((pkt[off + 2] << 8) | pkt[off + 3]);
        rr.ttl = ((uint32_t)pkt[off + 4] << 24) | ((uint32_t)pkt[off + 5] << 16) |
                 ((uint32_t)pkt[off + 6] << 8) | (uint32_t)pkt[off + 7];
        rr.rdlen = (uint16_t)((pkt[off + 8] << 8) | pkt[off + 9]);
        off += 10;
        if (off + rr.rdlen > pkt_len)
            return DG_ERR_PARAM;
        rr.rdata = pkt + off;
        off += rr.rdlen;
        if (cb) {
            int rc = cb(&rr, ud);
            if (rc != 0)
                return rc;
        }
    }
    return 0;
}
