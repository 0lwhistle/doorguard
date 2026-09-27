/*
 * test_net_info.c — 网络信息/配置纯逻辑测试(宿主)
 *
 * 覆盖:net_cfg 掩码转前缀与请求校验的完整判定表、net_info_read 的
 * 0.0.0.0 兜底语义(宿主机至少有 lo,无主接口时三地址必须全为
 * "0.0.0.0" 且 have_ip=false——2026-09-27 网络配置需求的核心契约)、
 * 有接口时地址为合法点分且非链路本地。apply()(真实 system 改网络)
 * 不在宿主测——那是推板后的板上验收项。
 */
#include "dg_test.h"

#include "net_cfg.h"
#include "net_info.h"

#include <stdio.h>
#include <string.h>

/* ---- net_cfg_mask_plen:点分掩码 → 前缀长度 ---- */

static void t_mask_plen(void)
{
    printf("[N1] 掩码 → 前缀长度(连续性校验)\n");
    DG_CHECK(net_cfg_mask_plen("255.255.255.0") == 24);
    DG_CHECK(net_cfg_mask_plen("255.255.0.0") == 16);
    DG_CHECK(net_cfg_mask_plen("255.0.0.0") == 8);
    DG_CHECK(net_cfg_mask_plen("255.255.255.255") == 32);
    DG_CHECK(net_cfg_mask_plen("128.0.0.0") == 1);
    DG_CHECK(net_cfg_mask_plen("255.255.255.128") == 25);

    DG_CHECK(net_cfg_mask_plen("255.0.255.0") == DG_ERR_PARAM);   /* 非连续 */
    DG_CHECK(net_cfg_mask_plen("0.0.0.0") == DG_ERR_PARAM);       /* 全零 */
    DG_CHECK(net_cfg_mask_plen("255.255.0") == DG_ERR_PARAM);     /* 缺段 */
    DG_CHECK(net_cfg_mask_plen("foobar") == DG_ERR_PARAM);
    DG_CHECK(net_cfg_mask_plen("") == DG_ERR_PARAM);
    DG_CHECK(net_cfg_mask_plen(NULL) == DG_ERR_PARAM);
}

/* ---- net_cfg_validate:请求合法性判定表 ---- */

static void req_fill(net_cfg_req_t *r, bool is_static, const char *ip,
                     const char *mask, const char *gw)
{
    memset(r, 0, sizeof(*r));
    r->is_static = is_static;
    if (ip)   snprintf(r->ip, sizeof(r->ip), "%s", ip);
    if (mask) snprintf(r->mask, sizeof(r->mask), "%s", mask);
    if (gw)   snprintf(r->gw, sizeof(r->gw), "%s", gw);
}

static void t_validate(void)
{
    net_cfg_req_t r;
    printf("[N2] 配置请求校验(DHCP 恒过/静态判定表)\n");

    DG_CHECK(net_cfg_validate(NULL) == DG_ERR_PARAM);

    req_fill(&r, false, "garbage", "", "");
    DG_CHECK(net_cfg_validate(&r) == DG_OK);          /* DHCP 不看地址 */

    req_fill(&r, true, "192.168.137.50", "255.255.255.0", "192.168.137.1");
    DG_CHECK(net_cfg_validate(&r) == DG_OK);
    req_fill(&r, true, "10.0.0.2", "255.0.0.0", "");
    DG_CHECK(net_cfg_validate(&r) == DG_OK);          /* 网关可空 */

    req_fill(&r, true, "0.0.0.0", "255.255.255.0", "");
    DG_CHECK(net_cfg_validate(&r) == DG_ERR_PARAM);   /* 全零地址 */
    req_fill(&r, true, "127.0.0.1", "255.255.255.0", "");
    DG_CHECK(net_cfg_validate(&r) == DG_ERR_PARAM);   /* 环回 */
    req_fill(&r, true, "169.254.1.9", "255.255.255.0", "");
    DG_CHECK(net_cfg_validate(&r) == DG_ERR_PARAM);   /* 链路本地 */
    req_fill(&r, true, "192.168.1", "255.255.255.0", "");
    DG_CHECK(net_cfg_validate(&r) == DG_ERR_PARAM);   /* 缩写拒收 */
    req_fill(&r, true, "192.168.137.50", "255.0.255.0", "");
    DG_CHECK(net_cfg_validate(&r) == DG_ERR_PARAM);   /* 非连续掩码 */
    req_fill(&r, true, "192.168.137.50", "", "");
    DG_CHECK(net_cfg_validate(&r) == DG_ERR_PARAM);   /* 缺掩码 */
    req_fill(&r, true, "192.168.137.50", "255.255.255.0", "0.0.0.0");
    DG_CHECK(net_cfg_validate(&r) == DG_ERR_PARAM);   /* 网关全零 */
    req_fill(&r, true, "192.168.137.50", "255.255.255.0", "999.1.1.1");
    DG_CHECK(net_cfg_validate(&r) == DG_ERR_PARAM);   /* 网关越界 */
}

/* ---- net_info_read:0.0.0.0 兜底语义 / 真实接口 ---- */

static bool dotted_ok(const char *s)
{
    int a, b, c, d;
    return sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) == 4 &&
           a >= 0 && a <= 255 && b >= 0 && b <= 255 &&
           c >= 0 && c <= 255 && d >= 0 && d <= 255;
}

static void t_net_info_read(void)
{
    net_info_addr_t a;
    printf("[N3] 地址快照(兜底 0.0.0.0 / 合法点分)\n");
    DG_CHECK(net_info_read(&a) == DG_OK);

    if (!a.have_ip) {
        /* 无任何外部接口(纯 lo):三地址必须统一 0.0.0.0 */
        DG_CHECK(strcmp(a.ip, "0.0.0.0") == 0);
        DG_CHECK(strcmp(a.mask, "0.0.0.0") == 0);
        DG_CHECK(strcmp(a.gw, "0.0.0.0") == 0);
    } else {
        DG_CHECK(dotted_ok(a.ip));
        DG_CHECK(dotted_ok(a.mask));
        DG_CHECK(dotted_ok(a.gw));                    /* 无默认路由时=0.0.0.0 */
        DG_CHECK(strcmp(a.ip, "0.0.0.0") != 0);
        DG_CHECK(strncmp(a.ip, "169.254.", 8) != 0);
        DG_CHECK(a.ifname[0] != '\0');
    }

    DG_CHECK(net_info_read(NULL) == DG_ERR_PARAM);
}

int main(void)
{
    t_mask_plen();
    t_validate();
    t_net_info_read();
    DG_TEST_EXIT();
}
