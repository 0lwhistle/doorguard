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

# 双字体:Latin 取 DejaVu(CJK 后备字体通常无 ASCII 字形),汉字取 Droid CJK
npx --yes lv_font_conv --no-compress --bpp 4 --size 16 \
  --font "${DG_FONT_LATIN:-/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf}" -r 0x20-0x7f \
  --font "$FONT_SRC" \
  --symbols " $(cat /tmp/dg_font_symbols.txt)" \
  --format lvgl --lv-include lvgl.h \
  --no-prefilter -o font/dg_font_cn_16.c
echo "生成 ui/font/dg_font_cn_16.c"
