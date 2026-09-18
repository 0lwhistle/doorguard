# web 上位机模块(HTTP + WebSocket)

板上内嵌 civetweb(spec-network §1),提供:登录、实时事件推送、门禁记录查询、
设备信息、NTP 触发、账号/口令管理、OTA 包接收。**不需要外网、不需要额外服务**,
设备上电即可用。

## 文件构成

| 文件 | 职责 |
|---|---|
| `web_server.c` | 路由、鉴权包装、响应工具、WebSocket 连接表与推送线程 |
| `web_auth.c/h` | 上位机凭据(device_config + PBKDF2)与登录风控(连错锁定) |
| `web_session.c/h` | token 表:签发/校验(滑动续期)/注销/容量驱逐 |
| `pages/` | 页面源文件(index.html / app.css / app.js)——真前端代码,可单独改 |
| `gen_pages.sh` | 把 `pages/` 打包成 `web_pages.c`(生成物随仓库提交) |
| `web_pages.c` | **生成物,不要手改** |

改页面流程:`编辑 pages/* → ./gen_pages.sh → 提交 pages/ 与 web_pages.c 两个变更`。
构建机不需要 python3 或前端工具链(生成物已入库)。

## 路由表

| 方法 | 路径 | 鉴权 | 说明 |
|---|---|---|---|
| GET | `/` `/app.css` `/app.js` `/favicon.ico` | 否 | 单页应用与资源 |
| POST | `/api/login` | 否 | → `{token,expires_in,user,pwd_default}` |
| POST | `/api/logout` | token | 注销当前 token |
| GET | `/api/device` | token | 版本/运行时长/用户数/日志数/IP/联网状态/mDNS/账号/NTP/存储 |
| GET | `/api/logs` | token | `from,to,user_id,page,page_size`(≤100)分页 |
| POST | `/api/ntp` | token | 异步触发(202),结果走 WebSocket |
| POST | `/api/account` | token+旧口令 | 改账号/口令,成功后**所有会话失效** |
| POST | `/api/ota/upload` | token | 流式收包 + sha256 校验(见 ota_service.h) |
| GET | `/api/ws` | `?token=` | WebSocket:实时推送认证事件与 NTP 结果 |

约束与理由:
- **改状态的接口一律 POST**,方法不符回 405(原先 GET 也能触发 NTP 校时,错误面过宽);
- 浏览器无法给 WebSocket 加自定义头,所以 WS 的 token 走查询串(其余接口一律 `X-Auth-Token`);
- 未授权 WS 连接**必须显式回 401 再拒**,否则客户端只看到连接被关,分不清"口令错"与"断网"。

## WebSocket 推送模型(为什么不是"客户端 ping 排水")

原实现:总线回调把消息入队,只有客户端周期性发 `ping` 时才在连接线程里排水——
本质是"靠客户端轮询掩盖跨线程写的不安全"。

现实现:`ws_pusher_thread` 常驻出队后广播。安全性靠两条同时成立:
1. civetweb 的 `mg_websocket_write` 内部会 `mg_lock_connection`,写不会交错;
2. 推送线程**持有连接表锁**做完整轮次写入,而连接销毁路径(`ws_close` 回调 →
   `close_connection`)必须先进回调摘除表项,因此写期间连接不可能被释放。

效果:客户端不发任何消息也能在毫秒级收到推送(验收:ws_test.py 全程不发消息)。

## 鉴权与风控

- 凭据存 `device_config`:账号明文(展示用)+ `web_pwd_salt`/`web_pwd_hash`(PBKDF2-SHA256,
  10000 次迭代,与设备端用户同款)。**不存明文、不落日志**。
- 首启自动生成 `admin/admin` 并置 `web_pwd_default=1`,UI/上位机据此提示"请尽快改密"。
- 账号/口令合法性走 `proto/valid.h`(与设备端用户同一份规则,账号按 uid 规则、口令按 pwd 规则),
  设备菜单与上位机两处入口不会漂移。
- 改凭据 = 吊销全部会话(`web_auth_set` 内部保证):旧 token 立即失效,防止"改完密码旧会话还在用"。
- 登录风控:同一来源连错 5 次锁定 60 秒(HTTP 429 + `Retry-After`),表 8 槽 LRU 且**不落盘**
  (重启即解锁——防止把管理员永久锁在门外;威胁模型是局域网脚本爆破,重启解锁不削弱防护)。

## 踩过的坑(改这个模块前先看)

1. **civetweb 在 OpenSSL 3 下 WebSocket 握手必崩**:`NO_SSL=1` 时它不包含 OpenSSL 头,
   却仍调用 `EVP_Digest`/`EVP_get_digestbyname`;缺原型 → 隐式声明把返回的指针截成 int →
   段错误(现象:日志停在"登录成功",客户端握手无响应)。修法见
   `third_party/civetweb/dg_openssl_shim.h` + CMake 的 `-include`(不改 vendored 源码)。
2. **`mg_send_http_error` 在 WebSocket 拒连路径会丢状态行**:它先置 `conn->status_code`,
   随后的 `mg_response_header_start` 认为"响应已开始"而不再发状态行,客户端只收到裸 body。
   WS 拒连必须自己写完整响应(`json_msg`)。
3. **`mg_set_request_handler("/", ...)` 在模式匹配阶段匹配一切**:所以 `/` 只能当兜底
   (内部按 URI 分派首页/404);把 `/api/**` 单独注册成 404 处理器永远不会被命中。
4. **`uptime_s` 曾是 `time(NULL) - 0`**(等于 epoch),上位机显示"1970 年至今";
   现在读 `/proc/uptime`,退化时用进程启动时间。
5. **WebSocket 原先完全没鉴权**:局域网内任何人都能连上收走全部门禁事件。现在 `?token=` 校验。

## 测试

```bash
dg-build-pc                     # 或 cmake --build build-pc
./tests/web/web_test.sh         # 57 项验收(自起服务、清沙箱库)
python3 tests/web/ws_test.py    # 只测 WebSocket(401/101/服务端主动推送)
python3 tests/web/ui_static_test.py  # 页面静态一致性(JS 引用的 id/接口必须真实存在)
```

`web_test.sh` 覆盖:静态资源与类型、鉴权(未登录/伪造/注销/改密后失效)、方法约束、
设备信息字段自洽、日志过滤与分页参数边界、NTP 异步 + WS 推送、账号口令修改、
OTA 上传闭环(200/422/400)、mDNS 报文、登录风控锁定。

## 未完成 / 已知边界

- 页面文案目前只有中文(设备端 `ui/lang/*.json` 那套 i18n 未接到 web 页面);要接的话
  在 `pages/` 里加一份文案表即可。
- 监控画面仍是占位:RTSP/MJPEG 需要与 `capture_service` 对接(取流统一走 capture,
  不另开摄像头链路),见 spec-network §1。
- web 端用户管理按 spec 仍不开放(接口未实现,返回 404)。
