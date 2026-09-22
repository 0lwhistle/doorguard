# 已弃用(2026-09-23,LVGL9 迁移 C1)

本目录仅剩 `drm.c/drm.h`(DRM dumb + legacy modeset)供 **LVGL 8.3 旧栈**使用:

- `DG_USE_LVGL9=OFF`(回退通道):仍编译 `drm.c`,行为不变;
- `DG_USE_LVGL9=ON`:**不编不链**——v9 走内置 Linux DRM 驱动
  (`third_party/lvgl9/src/drivers/display/drm/`,dumb x2 + atomic 翻转),
  板端封装见 `modules/display/display_drm_v9.c`。

上轮 video plane 实验(已回退,git f519b1e)的 atomic 经验已沉淀进
v9 驱动的使用方式;LVGL9 迁移完成后本目录随 8.3 旧栈一起择期移除。
