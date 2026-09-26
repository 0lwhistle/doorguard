#!/usr/bin/env bash
# 从 lang/*.json 收集全部字符,生成本地化位图字体(lv_font_conv,MIT 工具)
# 标签变化后重跑本脚本;生成的 .c 随仓库提交,测试机无需 node
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
out = "".join(sorted(c for c in chars if ord(c) > 0x7f))
sys.stdout.write(out)
PY

# 双字体:Latin 取 DejaVu(CJK 后备字体通常无 ASCII 字形),汉字取 Droid CJK。
# 多档字号:XS 16(密排预留)/SUB 26(次要提示)/CN 30(正文·列表·按钮)/
# TITLE 40(页面标题)——720×1280 屏的舒适观感下限(2026-09-27 字体调优)。
for size in 16 26 30 40; do
  npx --yes lv_font_conv --no-compress --bpp 4 --size "$size" \
    --font "${DG_FONT_LATIN:-/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf}" -r 0x20-0x7f \
    --font "$FONT_SRC" \
    --symbols " $(cat /tmp/dg_font_symbols.txt)" \
    --format lvgl --lv-include lvgl.h \
    --no-prefilter -o "font/dg_font_cn_${size}.c"
  echo "生成 ui/font/dg_font_cn_${size}.c"
done
