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
int db_user_del(const char *user_id);
int db_user_get(const char *user_id, user_rec_t *out);
int db_user_count(uint32_t *n);
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
