# UI 规格(LVGL,蓝白主题)

> 平台:同一份 UI 代码跑两处 —— RK3576(DRM 后端,板上)与 **PC 模拟器**(SDL2,调 UI 用)。
> 摄像头帧通过 camera HAL 抽象注入:板上取真实流,PC 端用视频文件/图片源回放。

## 1. 主题与组件规范

色值 token(定义 `ui/theme.h`,全项目只引用 token 不写裸色值):

| token | 值 | 用途 |
|---|---|---|
| `DG_COLOR_PRIMARY` | `0x1E88E5` | 主蓝:按钮底、标题栏、选中态 |
| `DG_COLOR_PRIMARY_DARK` | `0x1565C0` | 按钮按下态 |
| `DG_COLOR_BG` | `0xFFFFFF` | 页面底色 |
| `DG_COLOR_BG_LIGHT` | `0xE3F2FD` | 卡片/分区底 |
| `DG_COLOR_TEXT` | `0x212121` | 主文字 |
| `DG_COLOR_OK` | `0x2E7D32` | 成功(绿) |
| `DG_COLOR_ERR` | `0xC62828` | 失败(红) |
| `DG_COLOR_WARN` | `0xF9A825` | 脸框黄(检测中) |

- **每个按钮 = 图标 + label**:统一 `ui/widgets/dg_btn_create(icon_sym, label)` 创建,
  图标优先用 LVGL 内置 symbol(如 `LV_SYMBOL_SETTINGS`),不合适用户后续会换
- 所有 label 文本一律 `_("原文")` 包裹(`ui/i18n.h` 提供 `_()`)
- 弹窗统一走 `ui/widgets/dg_popup.h`:`dg_popup_success() / dg_popup_fail() / dg_popup_input() /
  dg_popup_choice()`,自动处理图层、自动关闭定时、回调

## 2. 多语言(i18n)

- 翻译文件:`ui/lang/zh-CN.json`、`en-US.json`(可扩展),**键 = 原文 label,值 = 翻译**
- `_()` 运行时查当前语言表;缺键回退原文并打 WARN 日志(方便发现漏翻)
- 语言切换(设备管理->语言)立即生效:发 event_bus 事件,各页面重刷静态文本

## 3. 三个页面

### 3.1 主页面(默认页,ST_NORMAL/ST_ADMIN_AUTH/ST_VERIFY 所在地)

- 背景:摄像头实时推流(NV12 → RGA/DRM 上屏,或模拟器下视频源);LVGL 只叠 UI 层
- 脸框 overlay:黄=检测到;绿=1:N/1:1 命中;红=失败/陌生人/黑名单(跟随人脸矩形)
- 下方两个按钮:**菜单**(左下,`LV_SYMBOL_SETTINGS`)、**验证**(右下,`LV_SYMBOL_OK`);
  行为见 spec-auth-business.md
- 弹窗全部高图层,推流不中断
- 顶部窄条(可选):时间、网络状态图标、NTP 已同步标记

### 3.2 待机页面

- 进入条件:菜单可设 **15~60s**(默认 30)无检测到人脸且无触摸操作
- 表现:基本全黑,屏幕中央只显示时间(HH:MM,每秒刷新)
- 唤醒:任何触摸 / 检测到人脸 → 回主页面并重置无操作计时

### 3.3 菜单页面(仅管理员,从 ST_MENU 进入)

四个入口(宫格,图标+label):

| 入口 | 功能 |
|---|---|
| 用户管理 | 添加/编辑/删除用户;字段:ID、姓名、密码(必填)、人脸/指纹/IC 录入、权限(普通/管理员/黑名单)、验证方式开关;冲突提示对照 spec-database 错误码 |
| 设备管理 | 语言切换;设备时间设置;**NTP 时间矫正按钮**(触发一次,显示成功/失败);网络配置(DHCP 开关、IP/掩码/网关,改动即生效并落库) |
| 门禁设置 | 预留页,先放:默认验证模式、待机超时(15~60)、开门时长、密码连错锁定参数(均落 device_config;后续按需求扩展) |
| 记录查询 | 按时间段(+可选用户 ID)查 access_logs,列表分页显示:时间/ID/姓名/方式/结果 |

通用:每页有返回按钮回上一级;页面栈深度固定,不做复杂导航。

## 4. 实现组织

```
door-guard/ui/
├── theme.h            色值/字号/间距 token
├── i18n.h/.c          _() 与语言表加载(json)
├── lang/              zh-CN.json en-US.json
├── widgets/           dg_btn / dg_popup / dg_kbd(数字键盘)/ dg_list
├── page_home.c        主页面(含验证状态机,逻辑见 spec-auth-business.md)
├── page_standby.c     待机页
├── page_menu.c        菜单主页
├── page_users.c       用户管理
├── page_device.c      设备管理
├── page_access_set.c  门禁设置
└── page_logs.c        记录查询
```

- 页面间跳转走页面管理器(`ui/page_mgr.h`:push/pop),事件经 event_bus 发布订阅
- UI 层**不做业务决策**:按钮事件发事件给 access/enroll 服务,结果经事件回 UI 渲染
- PC 模拟器构建:`cmake -DDG_SIM=ON`(SDL2 窗口 720×1280),与板上共用全部 ui/ 代码;
  模拟器专用输入源(视频/图片循环)由 camera HAL 的 sim 后端提供

## 5. 验收自检(UI 相关,每次提交 UI 改动前过一遍)

- 所有文本 `_("")` 包裹,zh-CN/en-US 两份 json 均有键(跑 i18n 一致性测试)
- 每个按钮有图标;蓝白主题;无裸色值
- 弹窗期间背景推流不冻结;5s/1.5s 超时行为与 spec-auth-business.md 一致
- 720×1280 竖屏无截断;模拟器与板上表现一致
