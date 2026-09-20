/*
 * crypto.c — 存储加密实现(OpenSSL EVP,接口契约见 crypto.h)
 */
#include "crypto.h"
#include "dg_log.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "[CRYPTO]";

static uint8_t s_device_key[32];
static bool s_key_ready = false;
static pthread_mutex_t s_key_mtx = PTHREAD_MUTEX_INITIALIZER;

int dg_crypto_init(const char *key_path)
{
    pthread_mutex_lock(&s_key_mtx);
    if (s_key_ready) {
        pthread_mutex_unlock(&s_key_mtx);
        return DG_OK;
    }
    if (!key_path || !*key_path) {
        pthread_mutex_unlock(&s_key_mtx);
        return DG_ERR_PARAM;
    }

    FILE *f = fopen(key_path, "rb");
    if (f) {
        /* 已有密钥:整读 32B;尺寸不符显式失败(重建将使旧特征不可解) */
        size_t n = fread(s_device_key, 1, sizeof(s_device_key), f);
        fclose(f);
        if (n != sizeof(s_device_key)) {
            DG_LOGE(TAG, "key file %s corrupted (size %zu != 32)", key_path, n);
            pthread_mutex_unlock(&s_key_mtx);
            return DG_ERR_IO;
        }
    } else {
        /* 首次开机:随机生成并落盘 0600 */
        if (RAND_bytes(s_device_key, sizeof(s_device_key)) != 1) {
            DG_LOGE(TAG, "RAND_bytes failed");
            pthread_mutex_unlock(&s_key_mtx);
            return DG_ERR_INTERNAL;
        }
        f = fopen(key_path, "wb");
        if (!f) {
            DG_LOGE(TAG, "cannot create key file %s", key_path);
            pthread_mutex_unlock(&s_key_mtx);
            return DG_ERR_IO;
        }
        /* 先限权再写:防短暂宽权限窗口 */
        chmod(key_path, 0600);
        if (fwrite(s_device_key, 1, sizeof(s_device_key), f) != sizeof(s_device_key)) {
            DG_LOGE(TAG, "short write to key file %s", key_path);
            fclose(f);
            pthread_mutex_unlock(&s_key_mtx);
            return DG_ERR_IO;
        }
        fclose(f);
    }
    s_key_ready = true;
    pthread_mutex_unlock(&s_key_mtx);
    return DG_OK;
}

void dg_crypto_deinit(void)
{
    pthread_mutex_lock(&s_key_mtx);
    dg_secure_wipe(s_device_key, sizeof(s_device_key));
    s_key_ready = false;
    pthread_mutex_unlock(&s_key_mtx);
}

int dg_pbkdf2_sha256(const char *pwd, const uint8_t *salt, uint32_t salt_len,
                     uint32_t iters, uint8_t out[DG_PWD_HASH_LEN])
{
    if (!pwd || !*pwd || !salt || salt_len == 0 || iters == 0)
        return DG_ERR_PARAM;    /* 迭代数下限由调用方用 DG_PBKDF2_ITERS 保证 */
    if (PKCS5_PBKDF2_HMAC(pwd, (int)strlen(pwd), salt, (int)salt_len,
                          (int)iters, EVP_sha256(), DG_PWD_HASH_LEN, out) != 1)
        return DG_ERR_INTERNAL;
    return DG_OK;
}

int dg_aes256_ctr(const uint8_t key[32], const uint8_t iv[16],
                  const uint8_t *in, size_t len, uint8_t *out)
{
    if (!key || !iv || (!in && len > 0) || !out)
        return DG_ERR_PARAM;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return DG_ERR_NO_MEMORY;
    int ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), NULL, key, iv) == 1;
    int outl = 0;
    if (ok)
        ok = EVP_EncryptUpdate(ctx, out, &outl, in, (int)len) == 1;
    int fin = 0;
    if (ok)
        ok = EVP_EncryptFinal_ex(ctx, out + outl, &fin) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok || (size_t)(outl + fin) != len)
        return DG_ERR_INTERNAL;
    return DG_OK;
}

int dg_feature_wrap(const uint8_t *plain, size_t len,
                    uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!s_key_ready)
        return DG_ERR_NOT_INIT;
    if (!plain || len == 0 || !out || !out_len)
        return DG_ERR_PARAM;
    if (out_cap < len + 16)
        return DG_ERR_PARAM;                    /* 需容纳 IV 前缀 */

    pthread_mutex_lock(&s_key_mtx);
    if (RAND_bytes(out, 16) != 1) {             /* 每次封装随机 IV:同明文不同密文 */
        pthread_mutex_unlock(&s_key_mtx);
        return DG_ERR_INTERNAL;
    }
    int rc = dg_aes256_ctr(s_device_key, out, plain, len, out + 16);
    pthread_mutex_unlock(&s_key_mtx);
    if (rc != DG_OK)
        return rc;
    *out_len = len + 16;
    return DG_OK;
}

int dg_feature_unwrap(const uint8_t *in, size_t len,
                      uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!s_key_ready)
        return DG_ERR_NOT_INIT;
    if (!in || len <= 16 || !out || !out_len)
        return DG_ERR_PARAM;
    if (out_cap < len - 16)
        return DG_ERR_PARAM;

    pthread_mutex_lock(&s_key_mtx);
    int rc = dg_aes256_ctr(s_device_key, in, in + 16, len - 16, out);
    pthread_mutex_unlock(&s_key_mtx);
    if (rc != DG_OK)
        return rc;
    *out_len = len - 16;
    return DG_OK;
}

int dg_constant_time_cmp(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++)
        diff |= a[i] ^ b[i];
    return diff == 0 ? 0 : 1;
}

void dg_secure_wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--)
        *v++ = 0;
}
