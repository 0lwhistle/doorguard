#!/usr/bin/env bash
# door-guard 环境脚本 —— source env/env.sh 后,env/bin 下的脚本直接敲名字可用
#
#   source <仓库>/env/env.sh     # 仓库内外均可,自动定位仓库根
#
# 可用脚本: dg-build / dg-deploy / dg-tc-install / dg-serial
# 可选环境变量(可写进 ~/.bashrc):
#   DG_TC_ROOT     交叉工具链根目录   (默认 ~/dg-toolchain)
#   DOORGUARD_IP   板子 IP            (dg-deploy 可省参数)
#   DOORGUARD_TTY  串口设备           (默认 /dev/ttyUSB0)

_DG_ENV_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export DOORGUARD_ROOT="$(dirname "$_DG_ENV_DIR")"

# PATH 去重:重复 source 不重复追加
case ":$PATH:" in
    *":$DOORGUARD_ROOT/env/bin:"*) ;;
    *) export PATH="$DOORGUARD_ROOT/env/bin:$PATH" ;;
esac

# 交叉工具链(dg-tc-install 装好后自动识别;候选位置依次探测)
if [ -z "${DG_TC_ROOT:-}" ]; then
    for _d in "$HOME/dg-toolchain" /home/olwhistle/dg-toolchain; do
        if [ -d "$_d/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin" ]; then
            export DG_TC_ROOT="$_d"
            break
        fi
    done
fi
export DOORGUARD_IP="${DOORGUARD_IP:-}"

echo "door-guard env 就绪"
echo "  DOORGUARD_ROOT = $DOORGUARD_ROOT"
echo "  脚本           = dg-build dg-deploy dg-tc-install dg-serial"
[ -n "$DG_TC_ROOT" ]   && echo "  DG_TC_ROOT     = $DG_TC_ROOT"
[ -n "$DOORGUARD_IP" ] && echo "  DOORGUARD_IP   = $DOORGUARD_IP"
true  # 保底返回 0:上面可选 echo 在变量为空时会短路成非零,连累 source 后的 && 链
