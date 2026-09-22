# LVGL9 迁移接口契约(三对话协作的唯一权威)

> 本文件是三个迁移对话之间的**接口契约 + 文件所有权 + 协作协议**。
> 冲突裁决顺序:本文件 > LVGL9.5 头文件实测 > 任何对话的"合理猜测"。
> 每条 API 映射以 `third_party/lvgl9` 头文件实测为准;实测不符时**以头文件为准**,
> 并在 `docs/lvgl9-migration-LOG.md` 记录差异(这是唯一允许偏离本表的情形)。

## 1. 文件所有权(绝对边界,越权 = 事故)

| 对话 | 独占可改 | 只读禁止改 |
|---|---|---|
| **C1 基础与显示层** | `third_party/lvgl9/`(新增)、`third_party/lv_drivers/`(标记弃用)、`CMakeLists.txt`、`cmake/`、`modules/display/`、`lv_conf9` 相关 | 其余一切 |
| **C2 UI 层迁移** | `ui/**`(pages/widgets/navigator/presenters/bridge/i18n/valid_ui/ui.c/port.c/ui/font) | third_party、modules/display、CMake(除非 C1 已提供开关且只需拨动,拨动也先记日志) |
| **C3 测试与验收** | `tests/**`、`docs/`(DEVLOG/性能报告)、板端部署操作 | 一切源码(发现问题→写日志→由对应对话修) |

共同只读:`proto/`、`services/`、`modules/camera|sqlite|net`、`configs/`(业务与契约,一律不改)。

## 2. 构建契约(C1 提供,C2/C3 消费)

- CMake 开关:**`DG_USE_LVGL9`**(ON/OFF)。C1 完成时置 ON 并默认 ON;OFF = 完整回退 8.3。
- v9 源码位置:`third_party/lvgl9`(include 根,`#include "lvgl.h"` 路径不变,ui 层源码零 include 改动)。
- lv_conf:9.5 conf 放 `third_party/lvgl9/lv_conf.h`;关键项:LV_COLOR_DEPTH 32、
  `LV_DRAW_SW_ASM_NEON` 开(aarch64)、字体 montserrat 14/20/28/48 + 沿用 8.3 conf 的字号集合、
  内存策略与 8.3 等价、LV_USE_OS = NONE(单线程主循环)。
- 显示后端:v9 Linux DRM(dumb,XRGB8888 720×1280)+ EVDEV 触摸;若 v9 驱动不适配,
  允许 C1 在 `modules/display/` 内自研 flush(禁改 display.h 签名)。
- `modules/display/display.h` 的既有 API 签名**冻结**(display_init/display_poll/
  display_set_touch_listener/display_touch_activity + video plane 桩四件)。

## 3. v8.3 → v9.5 API 映射表(本仓实际用到子集)

| v8.3 | v9.5 | 备注 |
|---|---|---|
| lv_disp_drv_t + lv_disp_draw_buf_init + lv_disp_drv_register | lv_display_create(W,H) + lv_display_set_flush_cb + lv_display_set_buffers(disp,buf1,buf2,字节数,LV_DISPLAY_RENDER_MODE_*) | render mode:我们用 PARTIAL 或 DIRECT,阶段 2 实测定 |
| flush_cb(disp_drv, area, color_p) | flush_cb(lv_display_t*, const lv_area_t*, uint8_t *px_map),末尾 lv_display_flush_ready(disp) | 擦除语义(同步后清脏区)必须原样保留,见 DEVLOG 残影条 |
| lv_scr_act() | lv_screen_active() | |
| lv_disp_get_default / lv_disp_set_bg_opa | lv_display_get_default / lv_display_set_bg_opa | |
| lv_obj_del | lv_obj_delete | |
| lv_img_create/set_src/set_antialias/set_pivot | lv_image_create/set_src/set_antialias/set_pivot | |
| lv_img_set_zoom(img, n)(256=1x) | lv_image_set_scale(img, n)(256=1x) | |
| lv_btn_create | lv_button_create | |
| lv_list_add_btn | lv_list_add_button | |
| lv_mem_alloc / lv_mem_free | lv_malloc / lv_free | |
| lv_memset_00 | lv_memzero | |
| lv_indev_drv + pointer read_cb | lv_indev_create(LV_INDEV_TYPE_POINTER,…) + lv_indev_set_read_cb | 触摸 fts_ts Type-B MT:优先 v9 evdev 驱动,不行复用自研 touch_evdev 喂 v9 |
| lv_theme_default_init(…) | 参数顺序变为 (display, color_primary, color_secondary, dark, font) | 以头文件为准 |
| lv_tick(系统提供) | lv_tick_set_cb(回调)(用 ui/port.c 的 dg_ui_tick_ms) | |
| lv_obj_add_event_cb / lv_event_get_target / LV_EVENT_* / lv_obj_set_style_* / lv_label_* / lv_obj_align*_center / LV_SYMBOL_* / lv_group_* / lv_async_call / lv_area | 同名沿用 | 仍需头文件抽查 |

**字体**:`ui/font/dg_font_cn_16.c` 是 v8 格式,须用 **v9 版 lv_font_conv** 重新生成
(ui/font/gen.sh 有原始命令,替换工具版本重跑);内置 montserrat_* 由 lv_conf 使能。

## 4. 协作协议(共享日志)

- 日志文件:`docs/lvgl9-migration-LOG.md`(追加写,最新在最上)。
- **开工**:先 `git pull`,再读日志最后 50 行;你的**前置对话未标 `[DONE]` 时**,
  只做你范围内的准备工作(不破坏他人成果),把"等待前置"写进日志后结束会话。
- **收工**:必须追加一条:`## <日期时间> C<N>-<角色> [状态: DONE/BLOCKED/WIP]`,
  写:做了什么/当前阻塞/交接事项(给下一个对话的必读信息);然后 `git push`。
- 状态流转:`C1 [DONE]` 是 C2 的开工条件;`C2 [DONE]` 是 C3 的开工条件。
- 遇到与本契约冲突的事实:以头文件/实测为准,改本文件需在日志里声明"契约修订"。

## 5. 回退与安全

- 基线 tag:**`lvgl9-baseline`**(= 3ada4ce 之后的 e9175d2,27fps 无残影稳定版)。
- 任一对话把仓库带入不可用状态:`git checkout lvgl9-baseline -- <你拥有路径>` 回滚自己的范围。
- 板端部署纪律:`dg-deploy` 后 **md5 核对运行进程**;卡死 `pkill -x door-guard`(勿 -f);
  七页走查重点:切页残影(上轮 video plane 的教训)、弹窗、中文渲染。
