/*
 * dg_openssl_shim.h — 只补 civetweb 需要的两个 OpenSSL 原型(强制包含)
 *
 * 背景(NO_SSL 配置下的实测坑):civetweb 用 NO_SSL=1 编译(门禁不需要 TLS),
 * 这个分支**不包含**任何 OpenSSL 头;但它的 WebSocket 握手在 OPENSSL_API_3_0
 * 分支里仍然调用 EVP_Digest()/EVP_get_digestbyname() 算 Sec-WebSocket-Accept。
 * 没有原型 → C 隐式声明 → 返回的 `const EVP_MD *` 被截成 int(64 位指针丢高 32 位)
 * → EVP_Digest 收到野指针 → **握手当场段错误**(现象:web 日志停在"登录成功",
 * WebSocket 连上即崩)。civetweb 目标带 -w,告警全被压掉,所以只能靠人发现。
 *
 * 为什么不直接 -include <openssl/evp.h>:OpenSSL 的 ssl.h 会 typedef
 * `struct ssl_st SSL`,与 civetweb 在 NO_SSL 下的占位 `typedef struct SSL SSL;`
 * 冲突(编译报 conflicting types)。所以这里只声明用到的两个函数,不引入任何
 * OpenSSL 头。原型与 OpenSSL 3.x 的 evp.h 严格一致(ENGINE* 用 void* 表示:
 * 实参恒为 NULL,ABI 上都是指针)。
 *
 * 用法(见 CMakeLists.txt):target_compile_options(civetweb PRIVATE
 *                                      -include .../dg_openssl_shim.h)
 */
#ifndef DG_OPENSSL_SHIM_H
#define DG_OPENSSL_SHIM_H

#include <stddef.h>

/* 与 OpenSSL 的 EVP_MD 前置声明兼容(不完整类型,只用于指针) */
struct evp_md_st;
typedef struct evp_md_st EVP_MD;

/* int EVP_Digest(const void *data, size_t count, unsigned char *md,
 *                unsigned int *size, const EVP_MD *type, ENGINE *impl); */
int EVP_Digest(const void *data, size_t count, unsigned char *md,
               unsigned int *size, const EVP_MD *type, void *impl);

/* const EVP_MD *EVP_get_digestbyname(const char *name); */
const EVP_MD *EVP_get_digestbyname(const char *name);

#endif /* DG_OPENSSL_SHIM_H */
