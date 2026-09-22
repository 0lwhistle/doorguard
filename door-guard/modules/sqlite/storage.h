/*
 * storage.h — 存储 HAL(spec-database §6 + §5 KV + 特征查重迭代器)
 *
 * 唯一允许接触 SQL 的模块;UI/业务层不得直接写 SQL。全部接口线程安全
 * (内部互斥);返回 0(DG_OK)成功、负数错误码(见 proto/err.h)。
 * 特征在内存结构 user_rec_t 中为明文,落库前自动加密、读出时自动解密。
 */
#ifndef DG_STORAGE_H
#define DG_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "err.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 生命周期 ---- */

/**
 * 打开/创建数据库并初始化 DDL(spec-database §1/§4/§5)与加密。
 * @param db_path  库文件路径(目录须存在;板上 /var/lib/door-guard/door-guard.db)
 * @param key_path 设备密钥路径(板上 /var/lib/door-guard/dg.key)
 * @return DG_OK / DG_ERR_IO / DG_ERR_DB / ...
 */
int storage_init(const char *db_path, const char *key_path);
void storage_deinit(void);

/* ---- 密码辅助:添加/改密前调用(哈希+盐写回 rec;密码本身不落任何盘) ---- */
int db_user_set_password(user_rec_t *rec, const char *plain_pwd);

/* ---- users(spec-database §6) ---- */

/** 添加用户:唯一性(uid/ic/特征查重)、上限 2000、密码必填全部在此校验 */
int db_user_add(const user_rec_t *in);
/** 更新:按 rec->user_id 定位;非零长度特征/非零哈希才覆盖对应字段 */
int db_user_update(const user_rec_t *in);
/* ---- 头像(spec-database:与特征同属生物特征数据,同密钥加密落库) ----
 * 独立接口而非塞进 user_rec_t:头像是 KB 级 BLOB,而 user_rec_t 在认证/检索
 * 热路径上每次都要整份拷贝——放进去等于每取一个用户多拷 10KB。 */

/** 写入/替换头像(JPEG 字节,≤32KB;len=0 表示清除)。落库前 AES-256-CTR 加密 */
int db_user_set_avatar(const char *user_id, const uint8_t *jpeg, size_t len);

/** 读头像(自动解密)。返回 DG_ERR_NOT_FOUND = 用户不存在**或**未录头像 */
int db_user_get_avatar(const char *user_id, uint8_t *out, size_t cap, size_t *out_len);

/** 清除人脸特征(len 置 0)——db_user_update 的"len=0=保留"语义无法表达
 *  清除,必须走本接口(2026-09-21 编辑页"清除人脸"落此坑) */
int db_user_clear_face(const char *user_id);
int db_user_del(const char *user_id);
int db_user_get(const char *user_id, user_rec_t *out);
int db_user_count(uint32_t *n);
/** 按 user_id 字典序列出全部用户 ID(用户管理列表数据源)。此前 UI 靠
 *  "1..2000 逐个探测数字 ID"拼列表,字母/前导零/超 2000 的合法 ID 永远
 *  不显示(计数却正常)——列表必须来自真实枚举而非猜测。
 *  ids 为调用方分配的 cap 个槽位;out_n 返回实际写入数(≤cap) */
int db_user_list_ids(char ids[][DG_UID_LEN], uint32_t cap, uint32_t *out_n);
/** 按权限计数(role 见 dg_role_t)。用途:菜单入口判断"系统里还有没有管理员"——
 *  一个都没有时必须免认证放行,否则新机/管理员被删光后菜单永远打不开 */
int db_user_count_role(int32_t role, uint32_t *n);
/** ID+密码验证;成功回填用户记录(密码错误计入 DG_ERR_WRONG_PASSWORD) */
int db_verify_password(const char *user_id, const char *pwd, user_rec_t *out);
int db_find_by_ic(const char *ic, user_rec_t *out);

/* ---- 特征比对迭代器:enroll 编排查重遍历库内特征(解密后明文交回调) ---- */

/** 回调返回非 0 中止遍历(如已判定重复);plain 在回调返回后立即擦除 */
typedef int (*dg_feature_iter_fn)(const char *user_id,
                                  const uint8_t *plain, uint16_t len, void *ud);
int db_user_iter_face(dg_feature_iter_fn fn, void *ud);
int db_user_iter_finger(dg_feature_iter_fn fn, void *ud);

/* ---- 人脸特征只读快照(M2②;verify 热路径直读,禁止逐帧查库) ---- */

typedef struct {
    char     user_id[DG_UID_LEN];
    int32_t  role;                         /**< dg_role_t(黑名单过滤由消费方做) */
    uint32_t auth_flags;                   /**< DG_AUTH_*(DG_AUTH_FACE 过滤由消费方做) */
    uint16_t face_len;                     /**< 恒 >0(无脸用户不入快照) */
    uint8_t  face_vec[DG_FEATURE_MAX];     /**< 明文(仅内存,不落盘) */
} dg_feat_ent_t;

typedef struct {
    uint32_t             count;
    const dg_feat_ent_t *ents;             /**< 只读数组,与快照同生命周期 */
} dg_feat_snap_t;

/**
 * 取人脸特征只读快照(启动全量装载,增删改增量同步)。
 * **必须成对**调用 storage_features_ro_done(),不可嵌套;持快照期间禁止
 * 调用 storage 其他接口。异常态(broken)返回 count=0:1:N 恒不命中,fail-closed。
 */
const dg_feat_snap_t *storage_features_ro(void);
/** 释放快照(与 storage_features_ro 成对) */
void storage_features_ro_done(void);

/**
 * 注入特征查重比较器(返回 1 = 判定重复)。
 * 人脸:enroll 编排注入 ROCKIVA 相似度比较;指纹:指纹算法比较。
 * 缺省为"解密后逐字节相等"基线,防止未注入时查重静默失效。
 * NULL 参数表示不修改对应槽位。
 */
typedef int (*dg_feature_cmp_fn)(const uint8_t *a, uint16_t a_len,
                                 const uint8_t *b, uint16_t b_len, void *ud);
int storage_set_feature_cmp(dg_feature_cmp_fn face_cmp,
                            dg_feature_cmp_fn finger_cmp, void *ud);

/* ---- access_logs(spec-database §6) ---- */

int db_log_append(const access_log_t *log);
/** 时间段+用户过滤分页查询;out->logs/max 由调用方提供 */
int db_log_query(const log_query_t *q, log_page_t *out);

/* ---- 存储占用(web 上位机设备信息展示;不暴露 SQL/路径给上层) ---- */

/** 库文件字节数与所在分区可用字节数(任一参数可为 NULL) */
int db_storage_stats(uint64_t *db_bytes, uint64_t *free_bytes);

/* ---- device_config KV(spec-database §5) ---- */

int db_config_set(const char *key, const char *value);
/** 键不存在返回 DG_ERR_NOT_FOUND(调用方决定默认值,不在此兜底) */
int db_config_get(const char *key, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* DG_STORAGE_H */
