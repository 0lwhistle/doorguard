#!/usr/bin/env bash
# gen_pages.sh — 把 pages/ 下的 HTML/CSS/JS 打包成 web_pages.c(C 字符串常量)
#
# 为什么生成而不是手写 C 字符串:页面有几百行动效与逻辑,手写转义字符串
# 既没法用编辑器高亮、也没法在浏览器里直接调;改成"真文件 + 生成"后,
# 前端可以脱离固件独立改(改完跑本脚本,生成的 .c 随仓库提交)。
# 依赖:python3(转义与分包)。用法:./gen_pages.sh
set -euo pipefail
cd "$(dirname "$0")"

python3 - <<'PY'
import io, os

SRC = [("DG_WEB_INDEX_HTML", "pages/index.html", "text/html"),
       ("DG_WEB_APP_CSS",    "pages/app.css",    "text/css"),
       ("DG_WEB_APP_JS",     "pages/app.js",     "application/javascript")]

def c_string(path):
    with io.open(path, encoding="utf-8") as f:
        text = f.read()
    out = []
    for line in text.split("\n"):
        esc = (line.replace("\\", "\\\\")
                   .replace('"', '\\"')
                   .replace("\t", "\\t"))
        out.append('    "%s\\n"' % esc)
    # 去掉最后一行多余的 \n(文件末尾换行不影响渲染,但保持一致更省事)
    if out:
        out[-1] = out[-1].replace('\\n"', '"')
    return "\n".join(out)

parts = []
for name, path, ctype in SRC:
    if not os.path.exists(path):
        raise SystemExit("缺少源文件: %s" % path)
    parts.append("/* %s (%s) */\nconst char *const %s =\n%s;\n"
                 % (path, ctype, name, c_string(path)))

header = """/*
 * web_pages.c — 内嵌页面(由 gen_pages.sh 从 pages/ 生成,**不要手改**)
 *
 * 改动流程:编辑 pages/index.html|app.css|app.js → ./gen_pages.sh → 提交两个
 * 文件(生成物随仓库提交,构建机无需 python3/前端工具链)。
 */
#include "web_pages.h"

"""
with io.open("web_pages.c", "w", encoding="utf-8") as f:
    f.write(header + "\n".join(parts))
print("生成 modules/net/web/web_pages.c")
PY
