#!/usr/bin/env python3
"""前端静态一致性检查(不需要浏览器,秒级;Vue 版)

为什么还需要它:前端是"构建进固件"的产物,单测覆盖逻辑但覆盖不到
"资源表/路由/体积"这些**交付面**;这类错在板上表现是白屏或 404,
而 C 编译器和 ctest 都发现不了。检查项:

  1. pages/ 与 web_pages.c 的资源表一致(改了前端忘记重新生成 → 立刻报错)
  2. 资源表里的路径 / MIME / 声明长度与实际文件一致,index.html 只引用表内资源
  3. 前端 api/endpoints.js 的每个接口路径都在 web_server.c 里有路由
  4. 分层纪律:fetch 只允许出现在 src/api/;components/ 不得 import stores/ 或 api/
     (模块化不是口号——这条把它变成可执行的约束)
  5. 产物体积预算(app.js ≤ 300KB、app.css ≤ 60KB):防止误引入 UI 框架
  6. 产物里没有外部 http(s) 资源(设备可能在无外网的局域网里)
用法:python3 frontend_check.py [door-guard 根]
"""
import os
import re
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..")
WEB = os.path.join(ROOT, "modules", "net", "web")
FRONT = os.path.join(WEB, "frontend")
PAGES = os.path.join(WEB, "pages")

MAX_JS = 300 * 1024
MAX_CSS = 60 * 1024

fails = []


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


# ---- 1. pages/ 与资源表一致 ----
page_files = []
sizes = {}
for dirpath, _dirs, files in os.walk(PAGES):
    for name in files:
        full = os.path.join(dirpath, name)
        rel = "/" + os.path.relpath(full, PAGES).replace(os.sep, "/")
        page_files.append(rel)
        sizes[rel] = os.path.getsize(full)
page_files.sort()

table = read(os.path.join(WEB, "web_pages.c"))
entries = re.findall(
    r'\{\s*"(/[^"]*)"\s*,\s*"([^"]*)"\s*,\s*asset_(\d+)\s*,\s*(\d+)\s*\}', table)
table_paths = sorted({p for p, _m, _i, _l in entries})
expected = sorted(set(page_files) | {"/"})       # "/" 是 index.html 的别名
if table_paths != expected:
    only_files = sorted(set(expected) - set(table_paths))
    only_tbl = sorted(set(table_paths) - set(expected))
    if only_files:
        fails.append(f"pages/ 里的文件没进资源表(忘了跑 gen_pages.sh?): {only_files}")
    if only_tbl:
        fails.append(f"资源表里的路径在 pages/ 不存在(陈旧产物?): {only_tbl}")
print(f"[1] 资源:{len(page_files)} 个文件,资源表 {len(entries)} 条")

# 声明长度必须等于实际字节数(长度写错会截断响应)
for p, _mime, _idx, ln in entries:
    if p in sizes and sizes[p] != int(ln):
        fails.append(f"资源表长度与实际不符: {p} 声明 {ln} 实际 {sizes[p]}")

# ---- 2. MIME 与 index.html 引用 ----
MIME_BY_EXT = {
    ".html": "text/html",
    ".css": "text/css",
    ".js": "javascript",
    ".svg": "image/svg+xml",
    ".json": "application/json",
}
for p, mime, _idx, _ln in entries:
    want = MIME_BY_EXT.get(os.path.splitext(p)[1].lower())
    if want and want not in mime:
        fails.append(f"{p} 的 MIME 可疑: {mime}")

index_html = read(os.path.join(PAGES, "index.html"))
refs = re.findall(r'(?:src|href)="(/[^"]+)"', index_html)
for r in refs:
    if r not in table_paths:
        fails.append(f"index.html 引用了不在资源表里的路径: {r}")
external = re.findall(r'(?:src|href)="(?:https?:)?//[^"]+"', index_html)
if external:
    fails.append(f"index.html 引用了外部资源(局域网/离线会失败): {external}")
print(f"[2] index.html 引用:{refs}")

# ---- 3. 前端接口路径 ↔ 服务端路由 ----
endpoints = read(os.path.join(FRONT, "src", "api", "endpoints.js"))
api_paths = sorted(set(re.findall(r"'(/api/[a-z/]+)'", endpoints)))
server = read(os.path.join(WEB, "web_server.c"))
routes = set(re.findall(r'mg_set_request_handler\(s_ctx,\s*"([^"]+)"', server))
routes |= set(re.findall(r'mg_set_websocket_handler\(s_ctx,\s*"([^"]+)"', server))
for p in api_paths:
    if p not in routes:
        fails.append(f"前端调用的接口在 web_server.c 里没有路由: {p}")
if not api_paths:
    fails.append("没解析到任何接口路径(endpoints.js 结构变了?)")
print(f"[3] 接口:{api_paths}")

# ---- 4. 分层纪律(把"模块化"变成可执行约束) ----
srcdir = os.path.join(FRONT, "src")
for dirpath, _dirs, files in os.walk(srcdir):
    for name in files:
        if not name.endswith((".js", ".vue")):
            continue
        full = os.path.join(dirpath, name)
        rel = os.path.relpath(full, srcdir).replace(os.sep, "/")
        body = read(full)
        if "fetch(" in body and not rel.startswith("api/"):
            fails.append(f"{rel} 直接调用 fetch:HTTP 出口只允许 src/api/")
        if rel.startswith("components/"):
            if re.search(r"from '\.\./stores/", body):
                fails.append(f"{rel} 引用了 stores/:展示组件必须靠 props/emits")
            if re.search(r"from '\.\./api/", body):
                fails.append(f"{rel} 引用了 api/:展示组件不得直接取数")
        if rel.startswith("stores/") and "from '../components/" in body:
            fails.append(f"{rel} 引用了组件:状态层不得依赖视图")
print("[4] 分层:fetch/stores/components 依赖方向已检查")

# ---- 5. 体积预算 ----
js_bytes = sizes.get("/assets/app.js", 0)
css_bytes = sizes.get("/assets/app.css", 0)
if js_bytes > MAX_JS:
    fails.append(f"app.js {js_bytes} 字节超过预算 {MAX_JS}(误引入大依赖?)")
if css_bytes > MAX_CSS:
    fails.append(f"app.css {css_bytes} 字节超过预算 {MAX_CSS}")
if js_bytes == 0:
    fails.append("pages/assets/app.js 缺失(前端未构建?)")
print(f"[5] 体积:app.js {js_bytes} B / 预算 {MAX_JS};app.css {css_bytes} B / 预算 {MAX_CSS}")

# ---- 6. 产物无外部资源 ----
# 注意:单纯出现 http:// 不算问题——SVG 的 xmlns(http://www.w3.org/2000/svg)
# 与 Vue 告警文案里的文档链接(https://vuejs.org)都是字符串常量,不会发起请求。
# 只查"真的会去取远端"的写法。
bundle = read(os.path.join(PAGES, "assets", "app.js"))
loaders = (
    re.findall(r"fetch\(\s*[`'\"](https?://[^`'\"]+)", bundle)
    + re.findall(r"\.open\(\s*[`'\"][A-Z]+[`'\"]\s*,\s*[`'\"](https?:[^`'\"]+)", bundle)
    + re.findall(r"import\(\s*[`'\"](https?://[^`'\"]+)", bundle)
    + re.findall(r"<script[^>]+src=[`'\"](https?://[^`'\"]+)", bundle)
    + re.findall(r"<link[^>]+href=[`'\"](https?://[^`'\"]+)", bundle)
)
if loaders:
    fails.append(f"产物会加载外部资源(离线环境会失败): {sorted(set(loaders))[:5]}")
hosts = sorted({h for h in re.findall(r"https?://([A-Za-z0-9._-]+)", bundle)})
print(f"[6] 离线:无外部加载;产物内出现的域名(仅字符串):{hosts}")

for f in fails:
    print("FAIL:", f)
print("前端静态检查:", "通过" if not fails else f"{len(fails)} 项失败")
sys.exit(1 if fails else 0)
