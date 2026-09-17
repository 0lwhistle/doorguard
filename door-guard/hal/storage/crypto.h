/*
 * crypto.h — 存储加密(PBKDF2-HMAC-SHA256 + AES-256-CTR,spec-database §3)
 *
 * 优先走系统 OpenSSL EVP(sysroot 实测 openssl 3.x;宿主测试同样依赖 libcrypto)。
 * 设备密钥 32B 随机生成,存 key_path(0600),内存按需读取,绝不落日志。
 */
#ifndef DG_CRYPTO_H
#define DG_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#include "err.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** PBKDF2 迭代次数:spec-database §3 下限即 spec 值;非业务参数不进配置文件,
 *  防误配置削弱安全基线 */
#define DG_PBKDF2_ITERS 10000u

/** 初始化:加载或首次生成设备密钥(文件 0600;父目录须已 0700)。
 *  key 文件存在但尺寸非 32B 视为损坏,返回 DG_ERR_IO(不静默重建——重建会使
 *  旧特征全部不可解,宁可显式失败)。 */
int dg_crypto_init(const char *key_path);
void dg_crypto_deinit(void);

/** PBKDF2-HMAC-SHA256:iters ≥ 10000(spec-database §3 下限) */
int dg_pbkdf2_sha256(const char *pwd, const uint8_t *salt, uint32_t salt_len,
                     uint32_t iters, uint8_t out[DG_PWD_HASH_LEN]);

/** 原始 AES-256-CTR(IV 显式传入,供已知向量对拍) */
int dg_aes256_ctr(const uint8_t key[32], const uint8_t iv[16],
                  const uint8_t *in, size_t len, uint8_t *out);

/** 特征封装:随机 16B IV 前缀 || 密文(设备密钥);out_cap ≥ len+16 */
int dg_feature_wrap(const uint8_t *plain, size_t len,
                    uint8_t *out, size_t out_cap, size_t *out_len);
/** 特征解封装:剥离前缀 IV 解密 */
int dg_feature_unwrap(const uint8_t *in, size_t len,
                      uint8_t *out, size_t out_cap, size_t *out_len);

/** 恒时比对(密码校验防时序侧信道) */
int dg_constant_time_cmp(const uint8_t *a, const uint8_t *b, size_t n);

/** 敏感明文用后擦除(避免编译器优化掉 memset) */
void dg_secure_wipe(void *p, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* DG_CRYPTO_H */
