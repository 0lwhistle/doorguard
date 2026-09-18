#!/usr/bin/env python3
"""内嵌页面静态一致性检查(不需要浏览器,秒级)。

为什么需要它:web 页面是"生成进固件"的字符串,写错 id 或漏一个路由,
在板上就是白屏/按钮点了没反应,而 C 编译器和 ctest 都发现不了。
本检查覆盖最常犯的三类错:
  1. JS 里 $('x')/getElementById('x') 引用的 id 在 HTML 里不存在(反之
     也报:HTML 有、JS 从不用 → 提示死元素)
  2. HTML 引用的资源(/app.css、/app.js、/favicon.ico)在 web_server 里
     没有对应路由 → 404
  3. CSS/HTML 括号与标签不平衡(生成脚本逐行转义,漏一个引号很难肉眼看出)
用法:python3 ui_static_test.py [door-guard 根目录]
"""
import os
import re
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..")
WEB = os.path.join(ROOT, "modules", "net", "web")

html = open(os.path.join(WEB, "pages", "index.html"), encoding="utf-8").read()
js = open(os.path.join(WEB, "pages", "app.js"), encoding="utf-8").read()
css = open(os.path.join(WEB, "pages", "app.css"), encoding="utf-8").read()
server = open(os.path.join(WEB, "web_server.c"), encoding="utf-8").read()

fails = []
warns = []

# ---- 1. id 引用一致性 ----
ids = set(re.findall(r'\bid="([^"]+)"', html))
refs = set(re.findall(r"\$\('([^']+)'\)", js))
refs |= set(re.findall(r"getElementById\('([^']+)'\)", js))
refs |= set(re.findall(r"url\(#([A-Za-z0-9_-]+)\)", html))   # SVG 内部引用
missing = sorted(refs - ids)
if missing:
    fails.append("JS 引用了 HTML 里不存在的 id: " + ", ".join(missing))
unused = sorted(ids - refs)
if unused:
    warns.append("HTML 里有 JS 未使用的 id(可能是死元素): " + ", ".join(unused))
print(f"检查 id:HTML {len(ids)} 个,JS 引用 {len(refs)} 个")

# ---- 2. 资源路由一致 ----
assets = set(re.findall(r'(?:href|src)="(/[^"]+)"', html))
routes = set(re.findall(r'mg_set_request_handler\(s_ctx,\s*"([^"]+)"', server))
for a in sorted(assets):
    if a not in routes and a != "/":       # "/" 由兜底处理器承担
        fails.append(f"HTML 引用的资源 {a} 没有服务端路由")
print(f"检查路由:页面引用 {sorted(assets)} / 服务端注册 {sorted(routes)}")

# ---- 3. 具体接口路径(JS fetch 的)都有路由 ----
api_paths = set(re.findall(r"api\('(/api/[a-z/]+)", js))
api_paths |= set(re.findall(r"fetch\('(/api/[a-z/]+)", js))
for p in sorted(api_paths):
    if p not in routes:
        fails.append(f"JS 调用的接口 {p} 没有服务端路由")
print(f"检查接口:JS 调用 {sorted(api_paths)}")

# ---- 4. 括号/标签平衡 ----
def balance(text, ch):
    return text.count(ch[0]) - text.count(ch[1])

if balance(css, "{}") != 0:
    fails.append(f"app.css 花括号不平衡({balance(css, '{}')})")
for tag in ("div", "section", "main", "header", "form", "table", "tbody", "thead",
            "tr", "td", "th", "label", "button", "span", "p", "h1", "h2", "pre",
            "script", "style", "svg"):
    op = len(re.findall(rf"<{tag}[\s>]", html))
    cl = len(re.findall(rf"</{tag}>", html))
    if op != cl:
        fails.append(f"HTML 标签 <{tag}> 不配对(开 {op} / 闭 {cl})")
if balance(js, "{}") != 0:
    fails.append(f"app.js 花括号不平衡({balance(js, '{}')})")

for w in warns:
    print("WARN:", w)
for f in fails:
    print("FAIL:", f)
print("ui 静态检查:", "通过" if not fails else f"{len(fails)} 项失败")
sys.exit(1 if fails else 0)
