#!/usr/bin/env bash
# build_frontend.sh — 一键重建前端并同步进固件资源
#
# 步骤:Vue 工程构建 → 覆盖 pages/(清掉旧产物,避免残留文件被编进固件)
#       → gen_pages.sh 生成 web_pages.c。
# 只在**改前端**时需要跑(需要 node/npm);只改后端 C 代码的构建机不需要。
#
# 用法:
#   ./build_frontend.sh            # 已有 node_modules 时直接构建
#   ./build_frontend.sh --install  # 先 npm ci(干净依赖)
set -euo pipefail
cd "$(dirname "$0")"

if [ "${1:-}" = "--install" ]; then
    ( cd frontend && npm ci )
fi

( cd frontend && npm run build )

rm -rf pages
mkdir -p pages
cp -r frontend/dist/. pages/

./gen_pages.sh

echo
echo "完成:pages/ 已更新(提交 pages/ 与 web_pages.c)"
find pages -type f | sort
