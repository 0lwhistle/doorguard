# web 上位机模块(HTTP + WebSocket + Vue 前端)

板上内嵌 mongoose web 服务(跑在 `modules/net/netcore` 统一事件循环,
spec-network §1),提供:登录、实时事件推送、门禁记录查询、
设备信息、NTP 触发、账号/口令管理、OTA 包接收。前端是 **Vue 3 单页应用**,
构建产物内嵌进固件——设备上电即用,不需要外网、不需要额外服务。

## 目录构成

```
services/web/
├── web_server.c/h     路由(表驱动)、鉴权包装、响应工具、WS 连接表(loop 私有)
├── web_auth.c/h       上位机凭据(device_config + PBKDF2)与登录风控
├── web_session.c/h    token 表:签发/校验(滑动续期)/注销/容量驱逐
├── frontend/          前端工程(Vue 3 + Vite;只在这里才需要 node/npm)
│   ├── src/
│   │   ├── api/           HTTP/WS 出口(client 统一注入 token、处理 401)
│   │   ├── stores/        模块级单例状态(session/device/events/toast)
│   │   ├── composables/   可复用逻辑(useClock/useCountUp)
│   │   ├── components/    纯展示组件(只吃 props、只发 emits)
│   │   ├── views/         页面(登录/概览/记录/账号/固件/监控)
│   │   ├── layouts/       AppShell(侧栏导航 + 顶栏 + 内容区)
│   │   ├── router/        路由表 + 登录守卫(history 可注入,便于单测)
│   │   └── styles/        tokens(与设备端 theme.h 同色)/ base / animations
│   ├── tests/             vitest:单元 + 集成 + 构建产物冒烟
│   └── package.json       依赖锁死精确版本(vue / vue-router / vite / vitest)
├── pages/              **入库的构建产物**(index.html + assets/ + favicon.svg)
├── build_frontend.sh   一键:npm build → 同步 pages/ → gen_pages.sh
├── gen_pages.sh        pages/ → web_pages.c(资源表,生成物入库)
├── web_pages.c/h       内嵌资源表(**不要手改**)
└── README.md
```

## 改前端 / 改后端的流程(两条路互不阻塞)

```bash
# 只改后端 C 代码 → 不需要 node
dg-build && dg-deploy -r <IP>

# 改前端(需要 node/npm)
cd services/web
./build_frontend.sh --install     # 只有依赖变了才加 --install(npm ci)
git add frontend pages web_pages.c
```

规矩:`pages/` 与 `web_pages.c` 是**入库的产物**,固件构建机不需要 node。
改了前端却忘了重新生成,`tests/web/frontend_check.py` 会立刻报错(它比对
`pages/` 与资源表是否一致);`tests/web/web_test.sh` 还会校验服务端吐出的字节与
仓库里那份**逐字节相同**(因此换了前端记得重新构建固件再验收)。

## 分层(可执行的模块化纪律)

```
views/           组装页面:调 stores、拼组件;不写 fetch、不写业务 DOM
  ↓
stores/          状态与编排:调 api、维护缓存/轮询/事件流
  ↓
api/             HTTP/WS 出口:token 注入、401 处理、错误归一
  ↓
components/      纯展示:只吃 props、只发 emits(不 import store / api / fetch)
```

这不是口号:`tests/web/frontend_check.py` 会扫源码并拒绝"组件里 import store""store
里 import 组件""api/ 之外出现 fetch"。样式同理——色值只允许用 `styles/tokens.css`
的变量(与设备端 `ui/theme.h` 同源,上下位机看起来是同一个产品)。

## 后端路由与鉴权

| 方法 | 路径 | 鉴权 | 说明 |
|---|---|---|---|
| GET | `/` 及未知路径 | 否 | 未命中资源表且非 `/api/` → 返回单页应用(SPA 回退) |
| GET | `/assets/app.js` `/assets/app.css` `/favicon.svg` | 否 | 资源表按**精确路径**命中,按各自 MIME 返回 |
| POST | `/api/login` | 否 | → `{token,expires_in,user,pwd_default}` |
| POST | `/api/logout` | token | 注销当前 token |
| GET | `/api/device` | token | 版本/运行时长/用户数/日志数/IP/联网/mDNS/账号/NTP/存储 |
| GET | `/api/logs` | token | `from,to,user_id,page,page_size(≤100)` 分页 |
| POST | `/api/ntp` | token | 异步触发(202),结果走 WebSocket |
| POST | `/api/account` | token+旧口令 | 改账号/口令,成功后**所有会话失效** |
| POST | `/api/ota/upload` | token | 流式收包 + sha256 校验 |
| GET | `/api/ws` | `?token=` | WebSocket:认证事件与 NTP 结果 |

约束与理由:
- **改状态的接口一律 POST**,方法不符回 405(原先 GET 也能触发 NTP 校时,错误面过宽);
- 浏览器无法给 WebSocket 加自定义头,所以 WS 的 token 走查询串(其余接口 `X-Auth-Token`);
- 未授权 WS 连接**必须显式回 401 再拒**,否则客户端只看到连接被关,分不清"口令错"与"断网";
- 前端用 **hash 路由**:设备端只提供固定资源表,不需要为前端路由配服务端回退。

## WebSocket 推送模型(2026-09-22 起)

总线回调只把消息入队,随后 `netcore_post` 让 **loop 线程**排空队列并逐连接
`mg_ws_send`。安全性从结构上成立:连接表是 loop 线程私有结构(无锁),
CLOSE 事件同线程摘除死连接,发送时连接必然存活;跨线程碰连接在
netcore 契约下不存在。(历史上 civetweb 时代靠"推送线程 + 连接表锁 +
连接锁"三条脆弱约定,已随库退役删除。)

前端对应 `src/api/ws.js`:退避重连;第二次失败顺手探一次接口——会话失效时 HTTP 层回
401,路由守卫把人送回登录页,而不是让用户对着一个不再更新的页面干等。

## 鉴权与风控

- **单会话策略(2026-09-22)**:同一时刻只允许一个管理员在线——登录成功即
  吊销其余全部会话;被踢方下一个请求/WS 重连得 401 回登录页(新登录必胜,
  崩溃浏览器不占坑)
- 凭据存 `device_config`:账号明文(展示用)+ PBKDF2-SHA256 盐/哈希(10000 迭代)。
  **不存明文、不落日志**;首启生成 `admin/admin` 并置 `pwd_default`,UI/上位机据此提示改密。
- 账号/口令合法性走 `proto/valid.h`(与设备端用户同一份规则):设备菜单与上位机两个入口
  不会漂移。前端只做"两次输入一致"这类纯前端判断,其余以服务端返回的错误为准。
- 改凭据 = 吊销全部会话(旧 token 立即失效)。
- 登录风控:同来源连错 5 次锁 60 秒(429 + Retry-After);锁定表内存维护、重启解锁
  ——防的是局域网脚本爆破,重启解锁不削弱防护,还避免把管理员永久锁在门外。

## 测试

```bash
# 前端逻辑与产物(需要 node/npm;不需要板子)
cd services/web/frontend && npx vitest run    # 44 项

# 交付面静态检查(不需要 node)
python3 door-guard/tests/web/frontend_check.py

# 接口级验收(自动起 PC 模拟器,打真接口)
door-guard/tests/web/web_test.sh                 # 63 项
```

- **vitest(44 项)**:`api/client`(token/401/参数编码)、`api/logs`(查询拼装)、
  `api/ws`(重连退避状态机)、`stores/{session,device,events}`、`utils/format`、
  组件(EventFeed/DataTable/AppField/StatusPill/StatValue)、路由守卫与 401 跳转,以及
  **`tests/integration/bundle.spec.js`:把 `pages/assets/app.js` 这份将编进固件的字节直接
  丢进 jsdom 执行**,跑通"挂载 → 登录 → 概览 → WebSocket 事件上屏"。
- **frontend_check.py**:资源表↔`pages/` 一致、MIME、`endpoints.js`↔`web_server.c` 路由对照、
  分层纪律、体积预算(app.js ≤300KB / app.css ≤60KB)、产物无外部资源。
- **web_test.sh**:静态资源(含"服务端字节与仓库逐字节一致")、鉴权与方法约束、日志
  过滤/分页边界、NTP 异步 + WS 主动推送、账号口令修改、OTA 闭环、mDNS 报文、登录风控。

## 踩过的坑(改这个模块前先看;1~3 为 civetweb 时代历史坑,迁移 mongoose 后不再适用,留作教训)

1. **civetweb 在 OpenSSL 3 下 WebSocket 握手必崩**:`NO_SSL=1` 时它不包含 OpenSSL 头,
   却仍调 `EVP_Digest`/`EVP_get_digestbyname`,缺原型 → 返回指针被截成 int → 段错误。
   修法见 `third_party/civetweb/dg_openssl_shim.h` + CMake `-include`(civetweb 带 `-w`,告警全被压掉)。
2. **WS 拒连丢状态行**:`mg_send_http_error` 先置 `conn->status_code`,后续 header 发送被跳过,
   客户端只收到裸 body → 必须自己写完整 401。
3. **`mg_set_request_handler("/")` 在模式匹配阶段匹配一切**:它只能当兜底(内部按 URI 分派);
   把 `/api/**` 单独注册成 404 处理器永远命中不到。
4. **`isPass(null)` 会被判成"通过"**:`Number(null) === 0` 踩的坑——门禁界面把"拒绝"
   显示成"通过"属事故级,已改为显式比较 `0`/`'0'`,并有单测锁死。
5. **列表 key 用 `ts + user_id` 拼字符串**:陌生人事件没有 `user_id` → key 变 `NaN`,
   Vue 复用错行(会打告警)→ 改由 store 打单调 id。
6. **vitest 里 `vi.resetModules()` 之后再 import 同一模块会拿到新实例**:store 是模块级
   单例,测试用另一实例改 token,应用侧守卫看不到 → 测试统一用 `mountApp()` 返回的同一代
   模块实例。
7. **应用侧按需 import 视图需要真实 I/O 轮次**:断言跳转用 `vi.waitFor` 轮询,不能假定
   一个微任务就到位;"表单提交登录"这条路径交给**构建产物**测试覆盖(jsdom 里直接执行
   app.js),比源码级更接近真实。

## 已知边界 / 未完成

- OTA 收包在统一事件循环内按 `ota_can_accept()` 限流喂入;极端慢盘顶到
  mongoose 3MB 收包上限会显式断连,客户端以 X-OTA-Offset 续传(有界失败)
- 页面文案只有中文(设备端 `ui/lang/*.json` 那套 i18n 未接入前端);要接的话在
  `frontend/src/` 加文案表即可。
- 监控画面仍是占位:RTSP/MJPEG 要与 `capture_service` 对接(统一走 capture,不另开链路)。
- web 端用户管理按 spec 仍不开放(接口未实现,返回 404)。
- 产物未做 gzip 内嵌(131KB JS,gzip 后 51KB):局域网直发可接受;若要省,可在构建时
  同时内嵌 `.gz` 并按 `Accept-Encoding` 返回。
