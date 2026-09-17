/*
 * err.h — door-guard 统一错误码(业务规格唯一来源)
 *
 * 约定(spec-database §6):接口"全部返回 0 成功,负数错误码"。
 * 分段分配,避免各模块自造冲突码:
 *   -1 ~ -19   通用/基础层
 *   -20 ~ -29  用户管理(spec-database §2)
 *   -30 ~ -39  验证业务(spec-auth-business)
 *   -40 ~ -49  网络/OTA(spec-network)
 * 错误码跨模块只经 proto/ 传递;UI 依据本头文件映射提示文案,不得自行编码。
 */
#ifndef DG_ERR_H
#define DG_ERR_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DG_OK = 0,

    /* ---- 通用/基础层 ---- */
    DG_ERR_PARAM      = -1,   /**< 参数非法(NULL/越界) */
    DG_ERR_NO_MEMORY  = -2,   /**< 内存分配失败 */
    DG_ERR_NOT_FOUND  = -3,   /**< 目标不存在 */
    DG_ERR_NOT_INIT   = -4,   /**< 模块未初始化 */
    DG_ERR_IO         = -5,   /**< 文件/设备 IO 失败 */
    DG_ERR_DB         = -6,   /**< SQLite 底层错误 */
    DG_ERR_STATE      = -7,   /**< 状态机非法迁移/当前态不允许 */
    DG_ERR_TIMEOUT    = -8,   /**< 超时 */
    DG_ERR_BUSY       = -9,   /**< 资源忙(单实例占用中) */
    DG_ERR_NETWORK    = -10,  /**< 网络不可达/失败 */
    DG_ERR_INTERNAL   = -11,  /**< 未分类内部错误 */

    /* ---- 用户管理(spec-database §2 唯一性/上限/必填) ---- */
    DG_ERR_NO_PASSWORD   = -20, /**< 新用户未设置密码,拒绝添加 */
    DG_ERR_DUP_UID       = -21, /**< user_id 重复 */
    DG_ERR_DUP_IC        = -22, /**< IC 卡号与其他用户重复 */
    DG_ERR_DUP_FACE      = -23, /**< 人脸特征 1:N 查重命中 */
    DG_ERR_DUP_FINGER    = -24, /**< 指纹特征 1:N 查重命中 */
    DG_ERR_USER_LIMIT    = -25, /**< 用户数达上限 2000 */

    /* ---- 验证业务(spec-auth-business §5) ---- */
    DG_ERR_WRONG_PASSWORD = -30, /**< 密码错误(计入连错) */
    DG_ERR_LOCKED         = -31, /**< 密码连错锁定中 */
    DG_ERR_AUTH_DISABLED  = -32, /**< 该用户全部验证方式已关闭 */
    DG_ERR_METHOD_DISABLED = -33, /**< 指定验证方式未开启 */
    DG_ERR_BLACKLIST      = -34, /**< 黑名单用户拒绝 */
    DG_ERR_MISMATCH       = -35, /**< 生物特征 1:1 不匹配 */
} dg_err_t;

/** 错误码名称(日志/上位机用);未知名返回 "UNKNOWN" */
const char *dg_err_name(dg_err_t err);

#ifdef __cplusplus
}
#endif

#endif /* DG_ERR_H */
