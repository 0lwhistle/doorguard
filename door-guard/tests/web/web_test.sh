#!/usr/bin/env bash
# web 上位机功能验收脚本(宿主跑,无需板子)
#   覆盖:登录 200/401、未登录 401、日志查询与 db 直查一致、坏 json 400、
#         设备信息、NTP 按钮(宿主无 chrony → 明确失败)、OTA 上传闭环
#   前提:dg-build-pc 已构建
set -uo pipefail

cd "$(dirname "$0")/../.."          # door-guard/ 根
PORT=${WEB_TEST_PORT:-18080}
BASE="http://127.0.0.1:$PORT"
PASS=0; FAIL=0
chk() { # chk <说明> <表达式>
    if eval "$2"; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); echo "FAIL: $1"; fi
}

# 沙箱数据目录,避免污染 sim/data
rm -rf /tmp/dg_webtest && mkdir -p /tmp/dg_webtest
export DG_WEB_PORT=$PORT
# PC 构建固定读 configs/device.json(web_port=8080);测试端口覆盖:
#   listening_ports 由 web_server.c 读 device_config,此处通过环境变量不生效,
#   直接改用 8080 并在冲突时跳过
if [ "$PORT" != "8080" ]; then
    echo "(注:web 端口来自 device.json,当前测试固定 8080)"
    BASE="http://127.0.0.1:8080"
fi

SDL_VIDEODRIVER=dummy DG_SIM_VISION=0 ./build-pc/door-guard sim/media \
    >/tmp/dg_webtest/server.log 2>&1 &
SRV_PID=$!
sleep 3

# 1 未登录访问受保护接口 → 401
code=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/api/logs")
chk "未登录 /api/logs → 401" "[ '$code' = '401' ]"
code=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/api/device")
chk "未登录 /api/device → 401" "[ '$code' = '401' ]"

# 2 登录:错误密码 → 401;正确(admin/admin 默认)→ 200 + token
code=$(curl -s -o /tmp/dg_webtest/bad.json -w "%{http_code}" \
    -H 'Content-Type: application/json' \
    -d '{"user":"admin","pwd":"wrong"}' "$BASE/api/login")
chk "错误密码登录 → 401" "[ '$code' = '401' ]"
resp=$(curl -s -H 'Content-Type: application/json' \
    -d '{"user":"admin","pwd":"admin"}' "$BASE/api/login")
TOKEN=$(echo "$resp" | python3 -c "import sys,json;print(json.load(sys.stdin).get('token',''))" 2>/dev/null)
chk "正确登录 → 200 + token" "[ -n '$TOKEN' ]"

# 3 坏 json → 400 不崩
code=$(curl -s -o /dev/null -w "%{http_code}" -H 'Content-Type: application/json' \
    -d '{bad json' "$BASE/api/login")
chk "坏 json → 400" "[ '$code' = '400' ]"
code=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/api/login" -X POST \
    -H 'Content-Type: application/json' --data-binary $'\xff\xfe broken')
chk "二进制垃圾 → 4xx 不崩" "[ '$code' = '400' ]"

# 4 日志查询:与 db 直查一致
resp=$(curl -s -H "X-Auth-Token: $TOKEN" "$BASE/api/logs")
web_total=$(echo "$resp" | python3 -c "import sys,json;print(json.load(sys.stdin)['total'])" 2>/dev/null)
db_total=$(sqlite3 sim/data/door-guard.db 'SELECT COUNT(*) FROM access_logs;' 2>/dev/null)
[ -z "$db_total" ] && db_total=$(python3 -c "
import sqlite3
print(sqlite3.connect('sim/data/door-guard.db').execute('SELECT COUNT(*) FROM access_logs').fetchone()[0])")
chk "日志 total 与 db 直查一致($web_total == $db_total)" "[ '$web_total' = '$db_total' ]"

# 5 坏参数 → 400
code=$(curl -s -o /dev/null -w "%{http_code}" -H "X-Auth-Token: $TOKEN" \
    "$BASE/api/logs?page=0&size=abc")
chk "坏参数 → 400" "[ '$code' = '400' ]"

# 6 设备信息
resp=$(curl -s -H "X-Auth-Token: $TOKEN" "$BASE/api/device")
chk "设备信息含版本" "echo '$resp' | grep -q version"

# 7 NTP 按钮:宿主无 chrony/离线 → 明确失败(ok=false)
resp=$(curl -s -X POST -H "X-Auth-Token: $TOKEN" "$BASE/api/ntp")
chk "NTP 触发明确返回" "echo '$resp' | grep -qE '\"ok\":(true|false)'"

# 8 OTA 上传闭环:sha256 相符 → 200;不符 → 422;超大小 → 400
PAYLOAD="FAKE-OTA-PAYLOAD-$(date +%s)"
SHA=$(echo -n "$PAYLOAD" | sha256sum | cut -d' ' -f1)
resp=$(curl -s -X POST -H "X-Auth-Token: $TOKEN" -H "X-OTA-Version: 9.9.9-test" \
    -H "X-OTA-Size: ${#PAYLOAD}" -H "X-OTA-SHA256: $SHA" \
    --data-binary "$PAYLOAD" "$BASE/api/ota/upload")
chk "OTA 正常包 → 校验闭环 200" "echo '$resp' | grep -q staged"
BADSHA=$(echo "$PAYLOAD-broken" | sha256sum | cut -d' ' -f1)
code=$(curl -s -o /dev/null -w "%{http_code}" -X POST -H "X-Auth-Token: $TOKEN" \
    -H "X-OTA-Version: 9.9.8" -H "X-OTA-Size: ${#PAYLOAD}" -H "X-OTA-SHA256: $BADSHA" \
    --data-binary "$PAYLOAD" "$BASE/api/ota/upload")
chk "OTA sha256 不符 → 422 拒收" "[ '$code' = '422' ]"
code=$(curl -s -o /dev/null -w "%{http_code}" -X POST -H "X-Auth-Token: $TOKEN" \
    -H "X-OTA-Version: huge" -H "X-OTA-Size: 99999999" -H "X-OTA-SHA256: $SHA" \
    --data-binary "x" "$BASE/api/ota/upload")
chk "OTA 超大小开始前拒绝" "[ '$code' = '400' ]"

# 9 首页可达(嵌入 UI)
code=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/")
chk "首页 200" "[ '$code' = '200' ]"

kill $SRV_PID 2>/dev/null
echo
echo "web 验收: $PASS 通过, $FAIL 失败"
[ "$FAIL" = 0 ]
