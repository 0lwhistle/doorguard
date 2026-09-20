# 数据库规格(SQLite)

> 库文件:`/var/lib/door-guard/door-guard.db`(目录 0700、库文件 0600;后续若分区方案调整,
> 数据目录迁至独立持久分区,见 spec-network.md OTA 一节)。访问一律走 `modules/sqlite`
> (dg_storage)封装,UI/业务层不得直接写 SQL。

## 1. users 表

```sql
CREATE TABLE users (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id     TEXT    NOT NULL UNIQUE,             -- 用户输入的业务 ID(字符串)
    user_name   TEXT    NOT NULL,
    face_vec    BLOB,                                -- AES-256-CTR 加密的人脸特征向量
    finger_vec  BLOB,                                -- AES-256-CTR 加密的指纹特征向量
    pwd_hash    BLOB    NOT NULL,                    -- PBKDF2-HMAC-SHA256(见加密策略)
    pwd_salt    BLOB    NOT NULL,                    -- 每用户 16B 随机盐
    ic_card     TEXT    UNIQUE,                      -- IC 卡号(明文,理由见 §3)
    role        INTEGER NOT NULL DEFAULT 0 CHECK (role IN (0,1,2)),  -- 0 普通 1 管理员 2 黑名单
    auth_flags  INTEGER NOT NULL DEFAULT 0,          -- bit0 人脸 bit1 指纹 bit2 密码 bit3 IC
    created_at  INTEGER NOT NULL,                    -- unix 秒
    updated_at  INTEGER NOT NULL
);
```

要点:

- **管理员可多个**:role=1 不限数量;管理员验证(菜单进入)只匹配 role=1
- **auth_flags 可全为 0**(用户存在但关闭全部验证方式):该用户不可被任何方式验证,
  验证入口直接失败(见 spec-auth-business.md);不等于删除
- **密码必填**(`NOT NULL`):添加用户时必须设置密码,否则拒绝添加 —— 业务层在 INSERT 前
  校验,不靠 DB 约束兜底
- **密码可重复**:验证语义是 ID+密码,重复无歧义;每用户独立盐,存哈希不存明文
- **用户上限 2000**:添加前 `SELECT COUNT(*)` 校验,超限返回失败(错误码 `ERR_USER_LIMIT`)

## 2. 唯一性规则(添加/编辑时校验,全部违反即失败,错误码区分)

| 冲突项 | 校验层 | 规则 |
|---|---|---|
| user_id | SQL UNIQUE + 业务预检 | 与任何已有用户重复 → `ERR_DUP_UID` |
| IC 卡号 | SQL UNIQUE + 业务预检 | 与任何已有用户重复 → `ERR_DUP_IC` |
| 人脸特征 | **业务层 1:N 查重** | 提取特征后与库内所有 face_vec 比对,相似度 ≥ 阈值(默认 0.90,进 device_config)→ `ERR_DUP_FACE` |
| 指纹特征 | **业务层 1:N 查重** | 同上,指纹算法比对分 ≥ 阈值 → `ERR_DUP_FINGER` |
| 密码 | 不校验 | 允许重复 |

**为什么特征查重不在 SQL 层**:特征向量是浮点/量化数据,"重复"是相似度语义而非逐字节相等,
SQL UNIQUE 无法表达;必须复用识别算法(ROCKIVA/指纹算法)做一次入库查重。这也是门禁行业的
"防一卡多注册/防同一生物特征多账号"标准做法。

错误码统一返回给 UI 与上位机,UI 弹对应红字提示(如"该人脸已绑定其他用户")。

## 2.1 字段合法性规则(唯一权威:`door-guard/proto/valid.c`)

| 字段 | 规则 | 违反错误码 |
|---|---|---|
| user_id | 3~31 字节;字母/数字/`-`/`_`;**首字符必须字母或数字**(避免 `-`/`_` 打头与命令行/URL 混用);区分大小写;不含空格 | `ERR_BAD_UID`(-26) |
| user_name | 1~63 字节;非空;无前导/尾随空格;不含控制字符(允许中文、空格、`·`、`-`) | `ERR_BAD_NAME`(-27) |
| password | 4~31 字节;**可见 ASCII**(0x21~0x7E),不含空格/制表/换行 | `ERR_BAD_PWD`(-28) |

- **两处执行同一份规则**:①设备 UI 输入弹窗按 OK 时即时校验(不合格就地红字提示,
  不提交、不占 5s 超时);②存储层 `db_user_add/db_user_update/db_user_set_password`
  强制校验,任何创建途径(设备 / 上位机 / 脚本 / 未来 API)都绕不过
- **密码为什么不能含空格**:设备键盘(数字+字母)本就不产生空格(空格键只在姓名输入给),
  且空格在日志/命令行里最容易被吞
- **中文姓名**:规则允许(上位机可录入);设备端键盘无输入法,**中文只能在设备上显示、
  不能在设备上输入**(设备上可输英文/数字姓名)
- 校验失败一律"拒绝写入 + 明确错误码",**不得静默截断或改写**用户输入

## 3. 加密存储策略

| 数据 | 策略 | 理由 |
|---|---|---|
| 密码 | PBKDF2-HMAC-SHA256,≥10000 次迭代,16B/用户随机盐,存 hash+salt | 口令行业标准;验证时同盐重算恒时比对 |
| 人脸/指纹特征 | AES-256-CTR,设备密钥加密后存 BLOB | 特征等同生物凭据,落盘必须加密;密钥首次开机随机生成,存 `/var/lib/door-guard/dg.key`(0600),内存中按需解密 |
| IC 卡号 | 明文存储;界面与日志显示掩码 `********1234` | 刷卡验证需按卡号 O(1) 索引查询,加密会退化为全表解密;库文件整体 0600 保护 + 显示脱敏是行业务实做法 |
| 门禁日志 | 明文,但 user_name 在导出接口可选脱敏 | 便于查询;不含凭据 |

密钥/库文件权限:目录 0700、文件 0600;绝不进 git、绝不经上位机接口导出。

## 4. access_logs 表(门禁日志)

```sql
CREATE TABLE access_logs (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    ts         INTEGER NOT NULL,                    -- unix 秒(验证发生时刻)
    user_id    TEXT,                                -- 命中用户;陌生人为 NULL
    user_name  TEXT,
    method     INTEGER NOT NULL,                    -- 0 人脸1:N 1 人脸1:1 2 指纹 3 密码 4 IC卡
    result     INTEGER NOT NULL,                    -- 0 通过 1 拒绝
    reason     INTEGER NOT NULL DEFAULT 0           -- 见下
);
CREATE INDEX idx_logs_ts   ON access_logs(ts);
CREATE INDEX idx_logs_user ON access_logs(user_id, ts);
```

reason 枚举:`0 成功` `1 陌生人` `2 黑名单` `3 密码错误` `4 特征不匹配` `5 用户不存在`
`6 该方式未开启` `7 超时未操作` `8 全部验证方式已关闭` `9 设备异常`。

- **每个验证动作都落一条**(成功/失败、任何方式),由 access_service 统一写入
- 菜单->记录查询 与 上位机共用同一查询接口:按时间范围 + 用户(可空)过滤,分页返回

## 5. device_config 表(设备配置 KV)

```sql
CREATE TABLE device_config (key TEXT PRIMARY KEY, value TEXT NOT NULL, updated_at INTEGER NOT NULL);
```

预置键:`language`、`standby_timeout_s`(15~60)、`face_dup_threshold`、`ota_url`、
`ntp_server`、`door_open_ms`、`pwd_fail_lock_n`(连续错 N 次锁定)、`pwd_fail_lock_s`。
网络配置(DHCP/静态 IP/掩码/网关)也存这里,由网络服务在开机与变更时应用。

## 6. 存储服务接口(modules/sqlite,示意)

```c
/* 全部返回 0 成功,负数错误码(见 door-guard/proto/err.h);线程安全(内部互斥) */
int  db_user_add(const user_rec_t *in);              /* 含全部唯一性/上限/密码校验 */
int  db_user_update(const user_rec_t *in);
int  db_user_del(const char *user_id);
int  db_user_get(const char *user_id, user_rec_t *out);
int  db_user_count(uint32_t *n);
int  db_verify_password(const char *user_id, const char *pwd, user_rec_t *out);
int  db_find_by_ic(const char *ic, user_rec_t *out);
int  db_log_append(const access_log_t *log);
int  db_log_query(const log_query_t *q, log_page_t *out);   /* 时间段+用户,分页 */
```
