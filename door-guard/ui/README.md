# ui/ — LVGL UI 子系统(蓝白主题,双端同源)

> 布局/组件规范见 `.agents/skills/door-guard-dev/references/spec-ui.md`(唯一权威)。

## 结构

```
ui/
├── theme.h        色值/字号/间距 token(spec-ui §1;全项目唯一色值来源)
├── i18n.h/.c      _() 翻译 + 语言表缓存 + 切换事件
├── lang/          zh-CN.json en-US.json(键=原文,值=译文)
├── font/
│   ├── gen.sh     字体生成:收集 lang/*.json 全部字符 → lv_font_conv → dg_font_cn_16.c
│   └── dg_font_cn_16.c   生成产物(入库,测试机无需 node)
├── port.c         LVGL 时基(LV_TICK_CUSTOM,dg_ui_tick_ms)
├── page_mgr.c     栈式页面管理(整建整删;栈深 4)
├── page_demo.c    组件冒烟页(Phase 6 由真实页面替代)
├── ui.c           引导:lv_init → display → i18n → 页面注册/打开
└── widgets/       dg_btn(图标+文本)/ dg_popup(success/fail/input/choice)
                    dg_kbd(数字键盘)/ dg_list
```

## 硬性纪律

- 色值只用 theme.h token,裸 0xRRGGBB=0(自动检查进 test_i18n)
- 所有界面文本一律 `_()`,裸中文 label=0(自动检查)
- UI 层不做业务决策:动作发事件(EV_*),结果经事件回 UI 渲染
- 新增/修改标签后:改 lang/*.json → 跑 `ui/font/gen.sh` 重新生成字体 →
  test_i18n 会强制校验双语言覆盖与字形覆盖

## 双端

- **PC 模拟器**:`dg-build-pc`(DG_SIM=ON,SDL2 720×1280)——调 UI 一律先过模拟器;
  `dg-build-pc -r [图片目录]` 直接跑,sim/media 为默认素材
- **板上**:同一份 ui/ 代码,显示走 DRM(Phase 8);LVGL 8.3 源码双端同源编译
  (third_party/lvgl + 自定义 lv_conf:CJK 字体、256KB 内存池)

## 使用示例

```c
/* 页面:注册 + 创建(对象树建在 parent 上,离页整删) */
static void page_x_create(lv_obj_t *parent) {
    lv_obj_t *btn = dg_btn_create(parent, LV_SYMBOL_SETTINGS, _("菜单"));
    lv_obj_add_event_cb(btn, on_menu, LV_EVENT_CLICKED, NULL);
    dg_popup_success(_("验证成功"), 3000, NULL, NULL);
}
static const dg_page_ops_t ops = { .name = "x", .create = page_x_create,
                                   .destroy = page_x_destroy };
page_mgr_register(&ops);
page_mgr_open("x");

/* 语言切换(立即生效,各页订阅 EVENT_UI_REFRESH_REQUEST 重刷) */
i18n_set_language("en-US");
```

## 测试

- `tests/test_i18n.c`:扫描源码 `_()` 键 → 双 json 覆盖 + 裸中文 label=0 +
  字体字形全覆盖(**此测试不过 Phase 永不通过**)
- `tests/test_widgets.c`:无头 LVGL 下四类 widget 创建/交互/弹窗生命周期
- 模拟器截图:docs/img/sim-phase5-widgets.png
