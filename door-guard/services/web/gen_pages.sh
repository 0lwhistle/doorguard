#!/usr/bin/env bash
# gen_pages.sh — 把 pages/(前端构建产物)打包成 web_pages.c 的资源表
#
# 为什么生成而不是手写 C 字符串:页面是 Vue 构建产物(130KB+),手写转义既
# 不可维护也无法在浏览器里调试。改成"真文件 + 生成"后,前端可独立开发,
# 生成物随仓库提交,固件构建机不需要 node/npm。
#
# 设计要点:
#   - 资源表(路径 → MIME + 内容):web_server 用一个通用处理器按路径查表,
#     以后加资源(图片/字体)不用改固件代码;
#   - 所有非 ASCII 字节转成八进制转义,生成物是纯 ASCII 的 C 源码,
#     不受编译器源码编码影响,也避免 \x 转义的贪婪匹配坑;
#   - 每 512 字节断开一次字符串(编译期长字面量太多会拖慢编译、也难 diff)。
# 用法:./gen_pages.sh     (产物:web_pages.c)
set -euo pipefail
cd "$(dirname "$0")"

[ -d pages ] || { echo "缺少 pages/(前端构建产物):先跑 ./build_frontend.sh" >&2; exit 1; }

python3 - <<'PY'
import io, os

MIME = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".ico": "image/x-icon",
    ".woff2": "font/woff2",
}
CHUNK = 512


def collect(root="pages"):
    """遍历 pages/,返回 [(请求路径, MIME, 相对文件)];路径一律以 / 开头"""
    out = []
    for dirpath, _dirs, files in os.walk(root):
        for name in sorted(files):
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            ext = os.path.splitext(name)[1].lower()
            out.append(("/" + rel, MIME.get(ext, "application/octet-stream"), full))
    return sorted(out, key=lambda x: x[0])


def c_bytes(data: bytes) -> str:
    """字节串 → C 字符串字面量(可拼接的多段)"""
    parts = []
    for i in range(0, len(data), CHUNK):
        chunk = data[i:i + CHUNK]
        esc = []
        for b in chunk:
            if b == 0x5C:
                esc.append("\\\\")
            elif b == 0x22:
                esc.append('\\"')
            elif b == 0x0A:
                esc.append("\\n")
            elif b == 0x0D:
                esc.append("\\r")
            elif b == 0x09:
                esc.append("\\t")
            elif 32 <= b < 127:
                esc.append(chr(b))
            else:
                esc.append("\\%03o" % b)
        parts.append('"%s"' % "".join(esc))
    if not parts:
        return '""'
    # 每行 4 段,便于阅读与 diff
    lines = ["        " + " ".join(parts[i:i + 4]) for i in range(0, len(parts), 4)]
    return "\n".join(lines)


assets = collect()
if not assets:
    raise SystemExit("pages/ 下没有文件")
if not any(a[0] == "/index.html" for a in assets):
    raise SystemExit("pages/ 缺少 index.html(前端构建产物不完整?)")

out = io.StringIO()
out.write("""/*
 * web_pages.c — 内嵌前端资源(由 gen_pages.sh 从 pages/ 生成,**不要手改**)
 *
 * 内容 = frontend/ 的 Vite 构建产物。改页面流程:
 *   cd frontend && npm ci && npm run build   # 或用 ../build_frontend.sh 一键
 *   ./gen_pages.sh                          # 重生成本文件
 *   git add pages/ web_pages.c              # 两个都提交
 * 资源查找在 web_server.c 的 handle_static()(按路径精确匹配)。
 */
#include "web_pages.h"

""")

# 索引页单独给一个常量:web_server 用它处理 "/" 与未知路径的单页应用回退
index_asset = next(a for a in assets if a[0] == "/index.html")
data = io.open(index_asset[2], "rb").read()
out.write("/* %s (%d 字节) */\n" % (index_asset[0], len(data)))
out.write("const char *const DG_WEB_INDEX_HTML =\n%s;\n\n" % c_bytes(data))

for i, (path, mime, full) in enumerate(assets):
    data = io.open(full, "rb").read()
    out.write("/* %s → %s (%d 字节) */\n" % (path, mime, len(data)))
    out.write("static const char asset_%d[] =\n%s;\n\n" % (i, c_bytes(data)))

out.write("/* 资源表:路径 → MIME + 内容(长度显式给出,避免每次请求 strlen) */\n")
out.write("const dg_web_asset_t DG_WEB_ASSETS[] = {\n")
for i, (path, mime, full) in enumerate(assets):
    size = os.path.getsize(full)
    out.write('    { "%s", "%s", asset_%d, %d },\n' % (path, mime, i, size))
# 根路径别名:前端用 hash 路由,所有页面都从 "/" 进入
out.write('    { "/", "%s", asset_%d, %d },\n'
          % (index_asset[1], assets.index(index_asset), os.path.getsize(index_asset[2])))
out.write("};\n\n")
out.write("const int DG_WEB_ASSET_COUNT = (int)(sizeof(DG_WEB_ASSETS) / sizeof(DG_WEB_ASSETS[0]));\n")

# 写临时文件再改名:中途失败不留半截 web_pages.c
text = out.getvalue()
tmp = "web_pages.c.tmp"
with io.open(tmp, "w", encoding="utf-8", newline="\n") as f:
    f.write(text)
os.replace(tmp, "web_pages.c")
print("生成 web_pages.c:%d 个资源, %d 字节" % (len(assets), len(text.encode("utf-8"))))
PY
