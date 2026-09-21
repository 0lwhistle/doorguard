# access 模块 — 验证状态机(auth_fsm)

> 行为规格唯一权威:`.agents/skills/door-guard-dev/references/spec-auth-business.md`。

## 定位

纯 C 事件驱动状态机,**不碰 LVGL、不碰 DB**:UI 层把触摸/识别结果翻译成
`fsm_event_t` 注入,FSM 输出动作(`FSM_ACT_*`)由 UI/服务层执行。
因此全部业务边界都可以在 ctest 里用事件序列逐条驱动(已做,见下)。

## 状态与标志位

```
ST_NORMAL ──菜单──▶ ST_ADMIN_AUTH ──5s无脸──▶ ST_NORMAL
   │验证               │管理员命中                │非管理员:红弹窗停留
   ▼                  ▼
ST_VERIFY ──各子步 5s 超时/取消──▶ ST_NORMAL
  (UID → 选方式 → 1:1人脸/指纹/密码/IC)
  成功/失败 ──▶ ST_RESULT(约3s)──▶ ST_NORMAL
ST_STANDBY(普通态空闲 30s)──触摸/人脸──▶ ST_NORMAL
```

标志位(match_enabled/popup_active/timer_seq)语义见 spec-auth-business §1,
FSM 内部维护;`match_enabled` 仅普通态且无弹窗时为真。

## 关键机制

- **timer_seq**:每次设置新定时器 seq+1;到期事件带回 seq,与内部不符即
  丢弃——杜绝"上个状态的 5s 超时把新状态拉回去"
- **密码连错锁定**:同 user_id 连错 N 次(默认5)锁 S 秒(默认60),内存态;
  `auth_fsm_pwd_locked()` 供 UI 在弹密码框前预检,锁定中直接提示不计次数
- **黑名单**:任何验证路径都失败(reason=2);1:N 检索含黑名单用户,
  命中即拒(spec §2.4)
- **日志唯一出口**:每个验证动作(任何方式/成败)都发 `FSM_ACT_WRITE_LOG`,
  页面写 access_logs 并广播 EV_AUTH_RESULT(Phase 7 收敛到 access_service)

## 初始化(业务参数全部来自 cfg)

```c
auth_fsm_init(&fsm, cfg_get()->door_open_ms, cfg_get()->standby_timeout_s,
              cfg_get()->menu_timeout_s, cfg_get()->pwd_fail_lock_n,
              cfg_get()->pwd_fail_lock_s, on_fsm_action, page_ctx);
```

## 测试(tests/test_auth_fsm.c + tests/test_verify_flow.c,ctest)

- `test_auth_fsm.c`(纯 FSM 事件序列):普通命中(开门+日志 result=0)/ 1.5s 未命中
  (reason=1)/ 黑名单(reason=2)/ 验证态挂起 1:N / UID 不存在·黑名单·全关方式 /
  子步 5s 超时 + 旧定时器失效 / 管理员三态 / 密码连错锁定 60s / 结果期忽略新请求 /
  待机进出 / 结果期开门只发一次 / **无管理员免认证进菜单、管理员入口验证过 role、
  取消流程、未开启方式被拒**
- `test_verify_flow.c`(服务层端到端,等价于把模拟器手点流程自动化):点验证 →
  `EV_UI_ASK_UID` → 提交 ID → `EV_UI_PICK_METHOD`(带 auth_flags)→ 选密码 →
  `EV_UI_INPUT_PWD`(带 uid)→ 提交密码 → `EV_UI_RESULT`(成功+开门 / 失败+原因)
  + 菜单入口两条(无管理员免认证;有管理员要认证,非管理员验证通过也进不去)

## 模拟器人工走通步骤(全流程验收,截图 docs/img/)

1. `dg-build-pc -r sim/media` 启动模拟器(可选 `DG_SIM_VISION=0` 关视觉 mock)
2. mock 开且普通模式:每 ~4s 人脸命中(user 10001)/超时失败交替;
   绿框+成功弹窗、红框+失败弹窗、开门日志均可见
3. 点"验证"→ 弹 ID 框输 10001 → OK → 弹方式选择(该用户开了人脸+密码)→
   选"密码"→ 弹密码框输 1234 → OK → 成功弹窗 + 开门日志
   (mock 在输入 ID/选方式期间不检索;选 1:1 人脸时模拟后端按目标 uid 回通过,
   三条方式都能走一遍)
4. 待机:静置 30s(可用短值配置)进黑屏时钟页;触摸/人脸唤醒
5. 菜单:**库里有管理员** → 要求管理员认证;**库里没有任何管理员 → 免认证直接进,
   并提示"未设置管理员,请先添加管理员"**(新机首次开机的情形)

> 自动化脚本走查注记:WSLg 合成点击(xdotool)与 LVGL 30ms 输入采样存在
> 丢事件,脚本全流程会被 5s 子步超时打断;人工鼠标操作不受影响。
> 每个界面状态已单独截图验证。
