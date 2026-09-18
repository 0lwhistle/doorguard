/*
 * mdns_wire.h — mDNS 报文编解码(RFC 6762/1035 的字节布局;纯函数)
 *
 * 为什么单独一层:报文是字节级协议,写错只能靠抓包才发现。把"取名字/写记录"
 * 做成不碰 socket 的纯函数后,可以对着字节布局写断言(tests/test_mdns_wire.c),
 * socket 层只剩收发与定时。压缩指针只在**解析**方向支持(应答侧不压缩,
 * 报文小,不值得为省几十字节引入指针正确性风险)。
 */
#ifndef DG_MDNS_WIRE_H
#define DG_MDNS_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDNS_PORT        5353
#define MDNS_MAX_NAME    128   /**< 含结束符;最长 "xxx._http._tcp.local" 绰绰有余 */
#define MDNS_MULTICAST   "224.0.0.251"

/* 记录类型 */
#define MDNS_TYPE_A      1
#define MDNS_TYPE_PTR    12
#define MDNS_TYPE_TXT    16
#define MDNS_TYPE_SRV    33
#define MDNS_TYPE_ANY    255

/* 类别(高 1 位在查询里是 QU=请求单播应答,在应答里是 cache-flush) */
#define MDNS_CLASS_IN      1
#define MDNS_CLASS_TOP     0x8000
#define MDNS_CLASS_MASK    0x7FFF

#define MDNS_FLAG_RESPONSE 0x8000   /**< QR=1 应答 */
#define MDNS_FLAG_AA       0x0400   /**< 权威应答 */

/** 报文写入器(溢出后转为 no-op 并置 overflow,调用方统一在 end 处判定) */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    uint16_t qd, an, ns, ar;
    bool     overflow;
} mdns_msg_t;

/** 开始构造(写占位头;计数在 mdns_msg_end 回填) */
void mdns_msg_begin(mdns_msg_t *m, uint8_t *buf, size_t cap, uint16_t id, uint16_t flags);
/** 回填各段计数;返回 DG_OK 或 DG_ERR_NO_MEMORY(溢出/写坏) */
int  mdns_msg_end(mdns_msg_t *m);

int mdns_msg_put_question(mdns_msg_t *m, const char *name, uint16_t type, uint16_t klass);
int mdns_msg_put_a(mdns_msg_t *m, const char *name, uint32_t ttl, bool flush,
                   const uint8_t ip[4]);
int mdns_msg_put_ptr(mdns_msg_t *m, const char *name, uint32_t ttl, const char *target);
int mdns_msg_put_srv(mdns_msg_t *m, const char *name, uint32_t ttl, bool flush,
                     uint16_t prio, uint16_t weight, uint16_t port, const char *target);
/** SRV 的 target 地址记录:进 **additional 段**(ARCOUNT),不带 cache-flush */
int mdns_msg_put_additional_a(mdns_msg_t *m, const char *name, uint32_t ttl,
                              const uint8_t ip[4]);
/** TXT:kv 为 "key=value" 字符串数组(每项写成一个长度前缀的字符串) */
int mdns_msg_put_txt(mdns_msg_t *m, const char *name, uint32_t ttl, bool flush,
                     const char *const *kv, int nkv);

/** 名字编码(不压缩):点分名 → 长度前缀序列 + 结束 0;返回写入字节数,<0 失败 */
int mdns_name_encode(const char *name, uint8_t *out, size_t cap);

/** 名字解码(支持压缩指针);返回消耗字节数,<0 失败 */
int mdns_name_decode(const uint8_t *pkt, size_t pkt_len, size_t off,
                     char *out, size_t cap);

/** 名字比较:大小写不敏感,忽略末尾点 */
bool mdns_name_equal(const char *a, const char *b);

/* ---- 解析方向(查询/探测冲突检测用) ---- */

typedef struct {
    char     name[MDNS_MAX_NAME];
    uint16_t type;
    uint16_t klass;      /**< 已掩掉 QU 位 */
    bool     unicast;    /**< QU 位置位:请求单播应答 */
} mdns_question_t;

/** 解析头 + 问题段;返回问题数(>=0),<0 为格式错误。
 *  id/flags 可为 NULL;qs 为 NULL 时只统计数量 */
int mdns_parse_query(const uint8_t *pkt, size_t pkt_len, uint16_t *id,
                     uint16_t *flags, mdns_question_t *qs, int max_q);

typedef struct {
    char           name[MDNS_MAX_NAME];
    uint16_t       type;
    uint16_t       klass;
    uint32_t       ttl;
    const uint8_t *rdata;   /**< 指向报文内部(不拷贝) */
    uint16_t       rdlen;
} mdns_rr_t;

/** 遍历应答/附加段记录(跳过问题段);cb 返回非 0 即停止并返回该值 */
int mdns_walk_rrs(const uint8_t *pkt, size_t pkt_len,
                  int (*cb)(const mdns_rr_t *rr, void *ud), void *ud);

#ifdef __cplusplus
}
#endif

#endif /* DG_MDNS_WIRE_H */
