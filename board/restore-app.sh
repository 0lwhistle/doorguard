#!/usr/bin/env bash
# restore-app.sh — 重刷系统 / 裸板后,一键恢复 door-guard 应用层
#
# 背景:door-guard 的应用层**不烘进固件**(B10 固件收编仍是待办),靠推送部署。
# 所以重刷 rootfs 后板上只剩 SDK 基础镜像:没有 S60doorguard、没有槽位、
# 没有 /etc/door-guard、没有 /userdata/doorguard/{models,cur_config.json}。
# dg-deploy 此时用不了(它假定应用已装、只做 A/B 换槽),必须先用本脚本种下
# 首次安装,之后日常改代码再走 dg-deploy。
#
# 用法(WSL 侧运行,资产都在 WSL):
#     bash board/restore-app.sh [板IP]        # 缺省 $DOORGUARD_IP 或 192.168.2.96
#
# 可覆盖的环境变量:
#     DG_BIN          应用二进制(缺省取 ~/dg-build/aarch64-*/door-guard 里最新的)
#     DG_DET_DIR      检测模型所在目录(缺省 /root/reinface/out;内含
#                     RetinaFace_rk3576_i8.rknn —— 该文件不在 git 里,只有这一份)
#     DG_REC_DIR      识别模型所在目录(缺省 /root/insightface/buffalo_l)
#     DG_AP     防欺骗模型目录(缺省 <仓库>/door-guard/models;两个 MiniFASNet)
#
# 幂等:可重复执行;已存在的槽位/资源被覆盖,不会重复建结构。
set -euo pipefail

IP="${1:-${DOORGUARD_IP:-192.168.2.96}}"
SSHOPTS=(-o StrictHostKeyChecking=accept-new -o ConnectTimeout=10)
sshx() { ssh "${SSHOPTS[@]}" "root@$IP" "$@"; }

# 仓库根(本脚本在 <repo>/board/ 下)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# ---- 资产定位 ----
DG_BIN="${DG_BIN:-$(ls -t $HOME/dg-build/aarch64-*/door-guard 2>/dev/null | head -1 || true)}"
DG_DET_DIR="${DG_DET_DIR:-/root/reinface/out}"
DG_REC_DIR="${DG_REC_DIR:-/root/insightface/buffalo_l}"
DG_AP="${DG_AP:-$ROOT/door-guard/models}"

# 二进制硬编码要求这四个模型名(见 services/vision/vision_rknn.c 与
# services/vision/README.md)。少一个,对应链路降级:
#   检测 RetinaFace_rk3576_i8.rknn / 识别 w600k_r50.rknn / 反欺骗 MiniFASNet ×2
need() { [ -f "$1" ] || { echo "缺少资产:$1"; exit 1; }; }
need "$DG_BIN"
need "$ROOT/board/rootfs-overlay/etc/init.d/S60doorguard"
need "$ROOT/door-guard/configs/default.json"
need "$DG_DET_DIR/RetinaFace_rk3576_i8.rknn"
need "$DG_REC_DIR/w600k_r50.rknn"
need "$DG_AP/2.7_80x80_MiniFASNetV2.rknn"
need "$DG_AP/4_0_0_80x80_MiniFASNetV1SE.rknn"

echo "==> 资产就绪"
echo "    二进制 $DG_BIN"
echo "    检测  $DG_DET_DIR/RetinaFace_rk3576_i8.rknn"

S="$(mktemp -d)"
trap 'rm -rf "$S"' EXIT

# ---- 板上目录 ----
echo "==> 建目录"
sshx 'mkdir -p /root/ui/lang /userdata/doorguard/models /etc/door-guard /var/lib/door-guard'

# ---- 推送 ----
echo "==> 推自启脚本(转 LF:Windows 检出可能是 CRLF,shebang 带 \r 上板即无法执行)"
tr -d '\r' < "$ROOT/board/rootfs-overlay/etc/init.d/S60doorguard" > "$S/S60doorguard"
scp "${SSHOPTS[@]}" -q "$S/S60doorguard" "root@$IP:/etc/init.d/S60doorguard"

echo "==> 推配置模板 / 语言包"
scp "${SSHOPTS[@]}" -q "$ROOT/door-guard/configs/default.json" "root@$IP:/etc/door-guard/default.json"
scp "${SSHOPTS[@]}" -q "$ROOT"/door-guard/ui/lang/*.json        "root@$IP:/root/ui/lang/"

echo "==> 推模型(约 92 MB)"
scp "${SSHOPTS[@]}" -q "$DG_DET_DIR/RetinaFace_rk3576_i8.rknn"  "root@$IP:/userdata/doorguard/models/"
scp "${SSHOPTS[@]}" -q "$DG_REC_DIR/w600k_r50.rknn"             "root@$IP:/userdata/doorguard/models/"
scp "${SSHOPTS[@]}" -q "$DG_AP/2.7_80x80_MiniFASNetV2.rknn"     "root@$IP:/userdata/doorguard/models/"
scp "${SSHOPTS[@]}" -q "$DG_AP/4_0_0_80x80_MiniFASNetV1SE.rknn" "root@$IP:/userdata/doorguard/models/"

# IC 卡驱动 .ko:内核模块不在 rootfs 镜像里(重刷即丢),来源是
# sync-driver.sh 的交付目录 ~/k7-deliverables/;S60doorguard 启动时从这里 insmod
DG_KO="${DG_KO:-$HOME/k7-deliverables/k7_rc522.ko}"
if [ -f "$DG_KO" ]; then
    echo "==> 推 IC 卡驱动 .ko"
    sshx 'mkdir -p /userdata/doorguard'
    scp "${SSHOPTS[@]}" -q "$DG_KO" "root@$IP:/userdata/doorguard/k7_rc522.ko"
else
    echo "!! 跳过 IC 卡驱动(缺 $DG_KO;在 WSL 跑 ~/k7-tools/sync-driver.sh 产出)"
fi

echo "==> 禁 lv_demo(它会占住屏幕)"
sshx 'bash -s' <<'REMOTE'
if [ -e /etc/init.d/S00-lv_demo ]; then
    mv /etc/init.d/S00-lv_demo /etc/init.d/disabled-S00-lv_demo
    echo "    S00-lv_demo 已禁用"
fi
killall lv_demo 2>/dev/null || true
REMOTE

# ---- 装非活动槽(不碰正在执行的二进制,否则 ETXTBSY)----
# 活动槽由 /root/door-guard 符号链接指出;已有应用在跑时,直写活动槽会
# "Text file busy"。按 A/B 纪律始终装另一槽,再原子切链。
cur=$(sshx 'basename "$(readlink /root/door-guard 2>/dev/null)" 2>/dev/null' || true)
if [ "$cur" = "dg_app.B" ]; then tgt=/root/dg_app.A; else tgt=/root/dg_app.B; fi
echo "==> 推应用二进制 → 非活动槽 $tgt(当前活动槽 ${cur:-无})"
scp "${SSHOPTS[@]}" -q "$DG_BIN" "root@$IP:$tgt"

echo "==> 落位 + 原子切链"
sshx "bash -s" <<REMOTE
set -e
chmod +x /etc/init.d/S60doorguard $tgt
ln -sf $tgt /root/door-guard.new && mv -f /root/door-guard.new /root/door-guard
echo "    活动槽 → \$(basename "\$(readlink /root/door-guard)")"
REMOTE

# ---- 校验 ----
echo "==> 校验 md5(本地产物 vs 板上新槽)"
lm=$(md5sum "$DG_BIN" | cut -d' ' -f1); lm=${lm#\\}
bm=$(sshx "md5sum $tgt" | cut -d' ' -f1)
[ "$lm" = "$bm" ] || { echo "md5 不一致!本地 $lm 板上 $bm"; exit 1; }
echo "    一致 $lm"

# ---- 启动 ----
# 注意:不能用 S60doorguard restart —— 它的 stop 分支末尾 `killall S60doorguard`
# 会按脚本名连"调用 restart 的父进程"一起 TERM 掉,导致后面的 start 永远执行
# 不到(实测:restart 后进程与 pidfile 全无)。dg-deploy 同样把 stop/start 拆成
# 两条独立命令。这里照做。
echo "==> 启动"
if [ -n "$cur" ]; then
    sshx '/etc/init.d/S60doorguard stop'  >/dev/null 2>&1 || true
    sleep 2
fi
sshx '/etc/init.d/S60doorguard start' || true
sleep 20
pid=$(sshx 'pidof door-guard' || true)
if [ -n "$pid" ]; then
    echo "==> door-guard 已运行(pid $pid,槽 $(sshx 'basename "$(readlink /root/door-guard)"'))"
    sshx 'grep -E "NPU\] 模型就绪|rknn 就绪" /var/log/door-guard.log | tail -6' || true
    echo "    日志:ssh root@$IP 'tail -f /var/log/door-guard.log'"
else
    echo "!! 进程未起来,看日志:ssh root@$IP 'tail -50 /var/log/door-guard.log'"
    exit 1
fi
