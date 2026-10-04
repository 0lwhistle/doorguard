#!/usr/bin/env bash
# 从 lang/*.json 收集全部字符,生成本地化位图字体(lv_font_conv,MIT 工具)
# 标签变化后重跑本脚本;生成的 .c 随仓库提交,测试机无需 node
#
# 运行时文本(用户姓名等)超出语言表字集 → 16/26/30 三档额外并入 GB2312 全集
# (symbols_cjk.txt,6763 字,2026-09-27:web 改中文名设备端显示空白的根因);
# 40px 标题档只渲染预置文案,保持小字集(全集 40px 会多 ~8MB rodata)。
set -euo pipefail
cd "$(dirname "$0")/.."
FONT_SRC="${DG_FONT_TTF:-/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf}"

python3 - <<'PY' > /tmp/dg_font_symbols.txt
import json, sys
chars = set()
for f in ("lang/zh-CN.json", "lang/en-US.json"):
    d = json.load(open(f))
    for k, v in d.items():
        chars.update(k)
        chars.update(v)
sys.stdout.write("".join(sorted(c for c in chars if ord(c) > 0x7f)))
PY
# 双字体:Latin 取 DejaVu(CJK 后备字体通常无 ASCII 字形),汉字取 Droid CJK。
# 多档字号:XS 16(密排预留)/SUB 26(次要提示)/CN 30(正文·列表·按钮·运行时姓名)/
# TITLE 40(页面标题)——720×1280 屏的舒适观感下限(2026-09-27 字体调优)。
# 16/26/30 并入 CJK 全集(sed 拼接去重);40 档仅语言表字集。
python3 - <<'PY' > /tmp/dg_font_symbols_cjk.txt
import json
chars = set(open("font/symbols_cjk.txt", encoding="utf-8").read())
for f in ("lang/zh-CN.json", "lang/en-US.json"):
    d = json.load(open(f))
    for k, v in d.items():
        chars.update(k)
        chars.update(v)
chars.discard("\n")
import sys
sys.stdout.write("".join(sorted(c for c in chars if ord(c) > 0x7f)))
PY

for size in 16 26 30 40; do
  sym="/tmp/dg_font_symbols.txt"
  [ "$size" != "40" ] && sym="/tmp/dg_font_symbols_cjk.txt"
  npx --yes lv_font_conv --no-compress --bpp 4 --size "$size" \
    --font "${DG_FONT_LATIN:-/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf}" -r 0x20-0x7f \
    --font "$FONT_SRC" \
    --symbols " $(cat "$sym")" \
    --format lvgl --lv-include lvgl.h \
    --no-prefilter -o "font/dg_font_cn_${size}.c"
  echo "生成 ui/font/dg_font_cn_${size}.c"
done

# 待机时钟专属档(2026-10-04):DejaVu Bold 仅数字+冒号(0x30-0x3A),
# 150px 大数字挂钟观感;独立 TTF 不并 CJK(无字母/汉字字形)
npx --yes lv_font_conv --no-compress --bpp 4 --size 150 \
  --font "${DG_FONT_CLOCK_TTF:-/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf}" \
  -r 0x30-0x3A \
  --format lvgl --lv-include lvgl.h \
  --no-prefilter -o "font/dg_font_clock.c"
echo "生成 ui/font/dg_font_clock.c"
