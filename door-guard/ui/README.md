# ui/ — LVGL UI 子系统(MVP 分层,学自 ESP32 ovs 工程)

> 布局/组件规范见 `.agents/skills/door-guard-dev/references/spec-ui.md`(唯一权威)。

## 分层与事件流向

```
入:后端 event_bus → bridge/(入 ui_events 队列)→ ui.c 全局泵(LVGL 线程,33ms)
      ├─ UI_EVT_GOTO_PAGE → navigator_switch(切页)
      └─ 其余 → navigator_dispatch_evt → 当前页 presenter.on_evt → pages setter 渲染
出:pages 点击 → bridge_btn/bridge_touch → event_bus → 服务层(FSM 决策后回流切页)
```

| 目录/文件 | 职责 | 禁止 |
|---|---|---|
| `ui.c` | 引导装配(theme→i18n→navigator→presenters→bridge→泵→首页)+ 全局事件泵 | 业务逻辑 |
| `navigator/` | 页面注册表 + 栈 + on_enter/on_exit/on_evt 生命周期 + reload(语言热切) | 业务 |
| `bridge/` | 唯一后端入口:事件入站编组、动作出站、_( ) 文案;只有本层 include 后端头 | 总线线程调 LVGL |
| `presenters/` | 每页一个 presenter:注册页面、on_evt 渲染内容、弹窗文案、导航决策 | — |
| `pages/` | 纯视图:建控件 + setter;点击转 bridge 动作 | include 后端头 |
| `widgets/` | 通用控件 dg_btn/dg_popup/dg_kbd/dg_list | 页面私有逻辑 |
| `theme.h` | 色值/字号/间距 token(spec-ui §1;全项目唯一色值来源) | — |
| `i18n.h/.c` + `lang/` | _() 翻译;字体经 font/gen.sh 生成(键=原文) | — |
| `port.c` | LVGL 时基(LV_TICK_CUSTOM) | — |

## 铁律(全部实测教训)

1. **总线线程禁止直接调 LVGL**——只经 bridge 入队(ui_events),泵出渲染;
   违者堆损坏崩溃(Phase 9 实录)。
2. **事件泵必须活在页面生命周期之外**(ui.c)——泵长在页面里,页面一销毁
   泵停,唤醒事件无人处理,屏幕卡死待机(2026-09-18 实录)。
3. **home↔standby 走 navigator_switch(栈内回退)**——纯压栈反复交替会撑爆
   页面栈;menu→users 这类前进用 navigator_push。
4. **ui/ 下注释不得用 ASCII 引号包中文**(`"xx"` 会被 i18n 检查判成字符串
   字面量),引用词用「xx」;用户可见文案一律 `_()`;页面日志用英文。
5. 语言热切换:`EV_UI_HINT(method=-5)` → presenter 调 `navigator_reload()`。

## 已知例外

- menu 四入口/子页返回键:纯 UI 导航,视图直调 navigator_push/back
  (无后端语义);业务返回(DG_BTN_BACK)仍走 bridge。
