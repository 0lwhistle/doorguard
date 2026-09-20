# storage — 存储 HAL(SQLite + 加密)

业务规格:`.agents/skills/door-guard-dev/references/spec-database.md`(唯一权威)。
本模块是**唯一允许接触 SQL 的代码**;UI/业务层一律经 `storage.h` 接口。

## Schema(spec-database §1/§4/§5 逐字一致)

```
┌──────────────────────────── users ────────────────────────────┐
│ id INTEGER PK AUTOINCREMENT                                   │
│ user_id   TEXT NOT NULL UNIQUE     ← 业务 ID(DUP_UID)        │
│ user_name TEXT NOT NULL                                        │
│ face_vec  BLOB    ← AES-256-CTR(设备密钥) 加密特征          │
│ finger_vec BLOB   ← 同上                                       │
│ pwd_hash  BLOB NOT NULL ← PBKDF2-HMAC-SHA256(10000 iters)    │
│ pwd_salt  BLOB NOT NULL ← 每用户 16B 随机盐                   │
│ ic_card   TEXT UNIQUE              ← 明文(展示掩码**1234)    │
│ role INTEGER CHECK(0,1,2)          ← 普通/管理员/黑名单       │
│ auth_flags INTEGER                 ← bit0 脸 bit1 指 bit2 密 bit3 IC │
│ created_at / updated_at INTEGER    ← unix 秒                  │
└────────────────────────────────────────────────────────────────┘
┌──────────────────── access_logs ──────────────────────────────┐
│ id PK / ts INTEGER / user_id TEXT(NULL=陌生人) / user_name   │
│ method(0 脸1:N 1 脸1:1 2 指 3 密 4 IC) / result(0 过 1 拒)  │
│ reason(0成功 1陌生 2黑名 3密错 4不匹配 5无用户 6方式关        │
│        7超时 8全关 9设备异常)                                 │
│ INDEX idx_logs_ts(ts); INDEX idx_logs_user(user_id, ts)       │
└────────────────────────────────────────────────────────────────┘
┌──────────────── device_config(KV) ────────────────────────────┐
│ key TEXT PK / value TEXT NOT NULL / updated_at INTEGER        │
│ 预置键:language / standby_timeout_s / face_dup_threshold /    │
│ ota_url / ntp_server / door_open_ms / pwd_fail_lock_n / ...   │
└────────────────────────────────────────────────────────────────┘
```

## 错误码(本模块返回;完整定义 proto/err.h)

| 码 | 名 | 触发点 |
|---|---|---|
| 0 | DG_OK | 成功 |
| -20 | NO_PASSWORD | 添加用户未设密码(pwd_hash 全零) |
| -21 | DUP_UID | user_id 与已有用户重复 |
| -22 | DUP_IC | IC 卡号与其他用户重复 |
| -23 | DUP_FACE | 人脸特征查重命中(比较器判定) |
| -24 | DUP_FINGER | 指纹特征查重命中 |
| -25 | USER_LIMIT | 用户数已达 2000(第 2001 个拒绝) |
| -30 | WRONG_PASSWORD | db_verify_password 密码不符 |
| -1/-3/-4/-6 | PARAM/NOT_FOUND/NOT_INIT/DB | 通用 |

## 关键设计("为什么")

- **单连接 + 全局互斥**:"查重→INSERT"必须原子,并发重复注册在门禁是
  安全事件;写入频次(验证日志 ≤ 每秒几条)远低于 SQLite 串行吞吐
- **特征查重不在 SQL 层**:相似度语义,逐行解密后交**比较器**(人脸
  ROCKIVA/指纹算法,由 enroll 编排经 `storage_set_feature_cmp` 注入);
  未注入时基线为解密后逐字节相等,防止查重静默失效
- **加密**:特征 AES-256-CTR(随机 IV 前缀,同明文不同密文)+ 设备密钥
  (`dg.key` 0600,首启随机生成);密码 PBKDF2-HMAC-SHA256 10000 迭代;
  IC 卡号明文(刷卡需 O(1) 索引,靠 0600+展示掩码,行业务实做法)
- **密码校验恒时比较**(dg_constant_time_cmp)防时序侧信道
- **WAL + busy_timeout 3s**:断电不损坏主库,并发读不受写阻塞
- **上限/唯一性先预检后 INSERT**:预检映射业务码,UNIQUE 约束兜底
- **库路径不外泄**:web 上位机要显示"存储占用",但库路径属本模块的部署细节;
  所以提供 `db_storage_stats(&db_bytes,&free_bytes)` 而不是把路径交给上层拼 stat

## 使用示例

```c
#include "storage.h"

storage_init("/var/lib/door-guard/door-guard.db", "/var/lib/door-guard/dg.key");

user_rec_t u = { .role = DG_ROLE_NORMAL, .auth_flags = DG_AUTH_FACE | DG_AUTH_PWD };
snprintf(u.user_id, sizeof(u.user_id), "10001");
snprintf(u.user_name, sizeof(u.user_name), "张三");
db_user_set_password(&u, "初始密码");            /* 盐+哈希写回 rec */
memcpy(u.face_vec, feat, feat_len); u.face_vec_len = feat_len;
int rc = db_user_add(&u);                        /* rc=DG_ERR_DUP_FACE 等 */

access_log_t log = { .ts = time(NULL), .has_user = true,
                     .method = DG_METHOD_FACE_1N,
                     .result = DG_RESULT_PASS, .reason = DG_REASON_OK };
snprintf(log.user_id, sizeof(log.user_id), "10001");
db_log_append(&log);

char lang[16];
if (db_config_get("language", lang, sizeof(lang)) == DG_ERR_NOT_FOUND)
    db_config_set("language", "zh-CN");          /* 默认值由调用方决定 */
```

## 测试

`tests/test_storage.c`(ctest):任务清单 6 组逐条覆盖——添加全错误码 +
2000 边界;密码对拍 PBKDF2 公开已知向量(1/2/4096 迭代);2500 条日志的
时间边界/按用户/分页(整除 100 与不整除 300)/倒序;配置 KV;特征
roundtrip + NIST SP 800-38A CTR-AES256 向量 + 同密码加盐唯一性;4×2500
并发日志零丢失。

## 运维校验

```bash
sqlite3 /var/lib/door-guard/door-guard.db .schema   # 与 spec-database 比对
sqlite3 /var/lib/door-guard/door-guard.db 'SELECT COUNT(*) FROM access_logs;'
```
