/*
 * types.h — door-guard 核心数据结构(字段对齐 spec-database §1/§4)
 *
 * 本文件是 users / access_logs 表在内存中的唯一投影:storage 层据此建表
 * 与读写,UI/服务层不得自定义重复结构(架构纪律)。字符串一律定长数组,
 * 便于按值入事件/落库,不做堆分配。
 */
#ifndef DG_TYPES_H
#define DG_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 容量常量(spec-database §1;业务参数进 device_config,这里是结构边界) ---- */

#define DG_USER_MAX         2000  /**< 用户上限;添加前 COUNT 校验 */
#define DG_UID_LEN          32    /**< user_id 业务 ID 字符串上限(含 '\0') */
#define DG_NAME_LEN         64    /**< user_name 上限(含 '\0') */
#define DG_IC_LEN           32    /**< IC 卡号字符串上限(含 '\0') */
#define DG_PWD_MAX_LEN      32    /**< 明文密码输入上限(仅传参用,不落库) */
/** 特征 BLOB 上限(加密后):人脸 = ArcFace-R50 512 维 float32 = 2048 B
 *  (自组 rknn 路线,2026-09-21 实测);ROCKIVA 时代为 512。
 *  users.features 是 BLOB 存长度按需使用,提上限免迁移;内存结构 user_rec_t
 *  随之变大(2000 用户全量特征缓存 ≈ 4 MB,M2 特征缓存可容纳) */
#define DG_FEATURE_MAX      2048
/** 头像 JPEG 上限(160×160 q80 实测 6~10KB,给足余量):storage 落库上限、
 *  vision 照片槽与 enroll 取件缓冲共用同一常量,避免三处魔数漂移 */
#define DG_AVATAR_JPEG_MAX  (32 * 1024)
#define DG_PWD_HASH_LEN     32    /**< PBKDF2-HMAC-SHA256 输出 256bit */
#define DG_PWD_SALT_LEN     16    /**< 每用户随机盐(spec-database §3) */

/* ---- 权限三级(spec-database §1 role CHECK 约束) ---- */

typedef enum {
    DG_ROLE_NORMAL = 0,   /**< 普通 */
    DG_ROLE_ADMIN   = 1,  /**< 管理员,可多个 */
    DG_ROLE_BLACKLIST = 2,/**< 黑名单:任何验证路径都失败 */
} dg_role_t;

/* ---- auth_flags 位定义(spec-database §1:bit0 人脸 bit1 指纹 bit2 密码 bit3 IC) ---- */

#define DG_AUTH_FACE    (1u << 0)
#define DG_AUTH_FINGER  (1u << 1)
#define DG_AUTH_PWD     (1u << 2)
#define DG_AUTH_IC      (1u << 3)
#define DG_AUTH_ALL     (DG_AUTH_FACE | DG_AUTH_FINGER | DG_AUTH_PWD | DG_AUTH_IC)

/* ---- users 表内存投影 ---- */

typedef struct {
    int64_t  id;                            /**< DB 自增主键(新增时置 0 忽略) */
    char     user_id[DG_UID_LEN];           /**< 业务 ID,UNIQUE */
    char     user_name[DG_NAME_LEN];
    uint8_t  face_vec[DG_FEATURE_MAX];      /**< AES-256-CTR 加密的人脸特征 */
    uint16_t face_vec_len;                  /**< 0 = 未录入人脸 */
    uint8_t  finger_vec[DG_FEATURE_MAX];    /**< AES-256-CTR 加密的指纹特征 */
    uint16_t finger_vec_len;                /**< 0 = 未录入指纹 */
    uint8_t  pwd_hash[DG_PWD_HASH_LEN];     /**< PBKDF2-HMAC-SHA256 */
    uint8_t  pwd_salt[DG_PWD_SALT_LEN];
    char     ic_card[DG_IC_LEN];            /**< 空串 = 未绑卡(DB 存 NULL) */
    int32_t  role;                          /**< dg_role_t */
    uint32_t auth_flags;                    /**< DG_AUTH_* 按位或;可为 0 */
    int64_t  created_at;                    /**< unix 秒 */
    int64_t  updated_at;                    /**< unix 秒 */
} user_rec_t;

/* ---- access_logs 方式/结果/原因枚举(spec-database §4) ---- */

typedef enum {
    DG_METHOD_FACE_1N = 0,  /**< 1:N 人脸(主页普通模式) */
    DG_METHOD_FACE_11 = 1,  /**< 1:1 人脸 */
    DG_METHOD_FINGER  = 2,
    DG_METHOD_PWD     = 3,
    DG_METHOD_IC      = 4,
} dg_auth_method_t;

typedef enum {
    DG_RESULT_PASS   = 0,
    DG_RESULT_REJECT = 1,
} dg_auth_result_t;

typedef enum {
    DG_REASON_OK             = 0, /**< 成功 */
    DG_REASON_STRANGER       = 1, /**< 陌生人 */
    DG_REASON_BLACKLIST      = 2, /**< 黑名单 */
    DG_REASON_WRONG_PWD      = 3, /**< 密码错误 */
    DG_REASON_MISMATCH       = 4, /**< 特征不匹配 */
    DG_REASON_NO_USER        = 5, /**< 用户不存在 */
    DG_REASON_METHOD_DISABLED = 6,/**< 该方式未开启 */
    DG_REASON_TIMEOUT        = 7, /**< 超时未操作 */
    DG_REASON_AUTH_DISABLED  = 8, /**< 全部验证方式已关闭 */
    DG_REASON_DEVICE_ERR     = 9, /**< 设备异常 */
} dg_auth_reason_t;

/* ---- access_logs 表内存投影 ---- */

typedef struct {
    int64_t id;                 /**< DB 自增主键 */
    int64_t ts;                 /**< unix 秒(验证发生时刻) */
    bool    has_user;           /**< false = 陌生人,DB 侧 user_id/user_name 为 NULL */
    char    user_id[DG_UID_LEN];
    char    user_name[DG_NAME_LEN];
    int32_t method;             /**< dg_auth_method_t */
    int32_t result;             /**< dg_auth_result_t */
    int32_t reason;             /**< dg_auth_reason_t */
} access_log_t;

/* ---- 日志查询(spec-database §4:时间段+用户过滤,分页返回) ---- */

typedef struct {
    int64_t ts_from;            /**< 起始 unix 秒(含);0 = 不限 */
    int64_t ts_to;              /**< 结束 unix 秒(含);0 = 不限 */
    char    user_id[DG_UID_LEN];/**< 空串 = 不过滤 */
    uint32_t page;              /**< 从 1 开始 */
    uint32_t page_size;         /**< 每页条数;0 视为非法回退默认 */
    bool    descending;         /**< true 按时间倒序 */
} log_query_t;

typedef struct {
    access_log_t *logs;         /**< 调用方提供数组与容量 */
    uint32_t max;
    uint32_t count;             /**< 本页实际条数 */
    uint32_t total;             /**< 过滤条件下总条数(分页 UI 用) */
} log_page_t;

/* ---- 位宽契约(编译期锁定,防字段改型悄悄破坏 DB/协议兼容) ---- */

_Static_assert(sizeof(((user_rec_t *)0)->role) == 4,
               "role 必须 32 位,对齐 users 表 INTEGER/CHECK(0,1,2)");
_Static_assert(sizeof(((user_rec_t *)0)->auth_flags) == 4,
               "auth_flags 必须 32 位:bit0~bit3 定义落在 uint32_t");
_Static_assert(DG_AUTH_IC <= 0x80000000u,
               "auth_flags 位定义不得超出 32 位");
_Static_assert(DG_PWD_HASH_LEN == 32 && DG_PWD_SALT_LEN == 16,
               "PBKDF2-HMAC-SHA256 32B 哈希 + 16B 盐,加密策略见 spec-database §3");

#ifdef __cplusplus
}
#endif

#endif /* DG_TYPES_H */
