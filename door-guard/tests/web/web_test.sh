#!/usr/bin/env bash
# web 上位机功能验收(宿主跑,无需板子)
#
# 覆盖:
#   页面静态一致性(JS 引用的 id/接口、HTML 引用的资源都要真实存在)
#   静态页与资源(/,/app.css,/app.js)可达且类型正确
#   鉴权:未登录 401、伪造 token 401、注销后失效、改凭据后旧 token 立即失效
#   方法约束:改状态接口不接受 GET(405)
#   设备信息字段自洽(运行时长必须是"运行时长",不是 epoch —— 历史 bug 回归)
#   日志查询:与 db 直查一致、倒序、按用户过滤、分页与参数校验
#   NTP 异步触发(202)+ WebSocket 推送全流程(见 ws_test.py:未授权 401 /
#     授权 101 / **客户端不发消息也收到服务端主动推送**)
#   账号/口令修改:旧口令必填、非法输入拒绝、成功后所有会话失效
#   登录风控:连错 5 次锁定 → 429(防局域网脚本爆破)
#   OTA 上传闭环(sha256 相符 200 / 不符 422 / 超大小 400)
#
# 前提:PC 模拟器已构建(见 door-guard/README)。脚本自己起服务、清沙箱库。
set -uo pipefail

cd "$(dirname "$0")/../.."          # door-guard/ 根
BASE="http://127.0.0.1:8080"
HOST=127.0.0.1
PORT=8080
PASS=0; FAIL=0
chk() { # chk <说明> <表达式>
    if eval "$2"; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); echo "FAIL: $1"; fi
}
# 从 stdin 的 JSON 取字段(点分支持嵌套;布尔输出 true/false)
jget() { python3 -c "
import sys,json
try:
    d=json.load(sys.stdin)
except Exception:
    print(''); sys.exit(0)
for k in '$1'.split('.'):
    if isinstance(d,dict) and k in d: d=d[k]
    else: print(''); sys.exit(0)
print('true' if d is True else ('false' if d is False else d))
"; }

if [ ! -x ./build-pc/door-guard ]; then
    echo "缺少 build-pc/door-guard:先构建 PC 模拟器(见 door-guard/README)"
    exit 1
fi

# 干净沙箱:清掉上次跑留下的库(否则凭据/日志残留会让断言不稳定)
rm -f sim/data/door-guard.db sim/data/dg.key
mkdir -p /tmp/dg_webtest

SDL_VIDEODRIVER=dummy DG_SIM_VISION=0 ./build-pc/door-guard sim/media \
    >/tmp/dg_webtest/server.log 2>&1 &
SRV_PID=$!
trap 'kill $SRV_PID 2>/dev/null' EXIT
sleep 3

if ! kill -0 $SRV_PID 2>/dev/null; then
    echo "服务未起来,日志:"; tail -20 /tmp/dg_webtest/server.log; exit 1
fi

echo "== 0. 页面静态一致性(不需服务) =="
if python3 tests/web/ui_static_test.py > /tmp/dg_webtest/ui_static.out 2>&1; then
    chk "页面 id/路由/括号一致" true
else
    chk "页面 id/路由/括号一致" false
    cat /tmp/dg_webtest/ui_static.out
fi

echo "== 1. 静态资源 =="
code=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/")
chk "首页 200" "[ '$code' = '200' ]"
curl -s "$BASE/" | grep -q "door-guard" && chk "首页含标题" true || chk "首页含标题" false
ct=$(curl -s -o /dev/null -w "%{content_type}" "$BASE/app.css")
chk "app.css 类型正确($ct)" "echo '$ct' | grep -q text/css"
ct=$(curl -s -o /dev/null -w "%{content_type}" "$BASE/app.js")
chk "app.js 类型正确($ct)" "echo '$ct' | grep -q javascript"
code=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/favicon.ico")
chk "favicon 200" "[ '$code' = '200' ]"
code=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/api/nonexistent")
chk "未知接口 404" "[ '$code' = '404' ]"

echo "== 2. 鉴权 =="
code=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/api/logs")
chk "未登录 /api/logs → 401" "[ '$code' = '401' ]"
code=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/api/device")
chk "未登录 /api/device → 401" "[ '$code' = '401' ]"
code=$(curl -s -o /dev/null -w "%{http_code}" -H "X-Auth-Token: bogus" "$BASE/api/device")
chk "伪造 token → 401" "[ '$code' = '401' ]"

echo "== 3. 方法约束(改状态接口不接受 GET) =="
code=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/api/login")
chk "GET /api/login → 405" "[ '$code' = '405' ]"
code=$(curl -s -o /dev/null -w "%{http_code}" -H "X-Auth-Token: x" "$BASE/api/ntp")
chk "GET /api/ntp → 405" "[ '$code' = '405' ]"

echo "== 4. 登录 =="
code=$(curl -s -o /dev/null -w "%{http_code}" -H 'Content-Type: application/json' \
    -d '{"user":"admin","pwd":"wrong"}' "$BASE/api/login")
chk "错误口令 → 401" "[ '$code' = '401' ]"
code=$(curl -s -o /dev/null -w "%{http_code}" -H 'Content-Type: application/json' \
    -d '{bad json' "$BASE/api/login")
chk "坏 json → 400" "[ '$code' = '400' ]"
resp=$(curl -s -H 'Content-Type: application/json' \
    -d '{"user":"admin","pwd":"admin"}' "$BASE/api/login")
TOKEN=$(echo "$resp" | jget token)
chk "默认凭据登录成功并拿到 token" "[ -n '$TOKEN' ]"
chk "登录响应含有效期" "echo '$resp' | grep -q expires_in"
chk "首启标记为默认口令" "[ \"\$(echo '$resp' | jget pwd_default)\" = 'true' ]"

echo "== 5. 设备信息 =="
resp=$(curl -s -H "X-Auth-Token: $TOKEN" "$BASE/api/device")
uptime=$(echo "$resp" | jget uptime_s)
chk "含版本号" "echo '$resp' | jget version | grep -q ."
chk "运行时长是运行时长而非 epoch($uptime)" "[ -n '$uptime' ] && [ '$uptime' -lt 1000000000 ]"
chk "含用户数" "echo '$resp' | grep -q '\"users\":'"
chk "含 mDNS 地址" "echo '$resp' | jget mdns_url | grep -q 'http://'"
chk "含 mDNS 主机名" "echo '$resp' | jget mdns_host | grep -qE '^[a-z0-9-]+'"
chk "含 web 端口" "echo '$resp' | jget web_port | grep -qE '^[0-9]+'"
chk "当前账号为 admin" "[ \"\$(echo '$resp' | jget web_user)\" = 'admin' ]"
chk "含会话数" "echo '$resp' | jget sessions | grep -qE '^[0-9]+'"
chk "含 NTP 状态块" "echo '$resp' | grep -q '\"ntp\":'"
chk "含联网状态" "echo '$resp' | jget online | grep -qE '^(true|false)$'"

echo "== 6. 日志查询(先造 3 条数据) =="
python3 - <<'PY'
import sqlite3, time
now = int(time.time())
rows = [(now - 10, '10001', '张三', 0, 0, 0),
        (now - 5,  '10002', '李四', 3, 1, 3),
        (now - 2,  None,    None, 0, 1, 1)]
c = sqlite3.connect('sim/data/door-guard.db')
c.executemany("INSERT INTO access_logs(ts,user_id,user_name,method,result,reason)"
              " VALUES(?,?,?,?,?,?)", rows)
c.commit()
print("造数完成:", c.total_changes)
PY
resp=$(curl -s -H "X-Auth-Token: $TOKEN" "$BASE/api/logs")
web_total=$(echo "$resp" | jget total)
db_total=$(python3 -c "
import sqlite3
print(sqlite3.connect('sim/data/door-guard.db').execute('SELECT COUNT(*) FROM access_logs').fetchone()[0])")
chk "日志 total 与 db 直查一致($web_total == $db_total)" "[ '$web_total' = '$db_total' ]"
newest=$(echo "$resp" | python3 -c "
import sys,json
d=json.load(sys.stdin)['logs']
print(d[0]['user_name'] if d else '')" 2>/dev/null)
chk "按时间倒序(最新在前:$newest)" "[ '$newest' = '陌生人' ]"
chk "字段含方式与结果" "echo '$resp' | grep -q method_name && echo '$resp' | grep -q '\"result\"'"
resp=$(curl -s -H "X-Auth-Token: $TOKEN" "$BASE/api/logs?user_id=10001")
chk "按用户过滤只回 1 条" "[ \"\$(echo '$resp' | jget total)\" = '1' ]"
code=$(curl -s -o /dev/null -w "%{http_code}" -H "X-Auth-Token: $TOKEN" "$BASE/api/logs?page=0")
chk "page=0 → 400" "[ '$code' = '400' ]"
code=$(curl -s -o /dev/null -w "%{http_code}" -H "X-Auth-Token: $TOKEN" "$BASE/api/logs?page_size=999")
chk "page_size 超上限 → 400" "[ '$code' = '400' ]"
code=$(curl -s -o /dev/null -w "%{http_code}" -H "X-Auth-Token: $TOKEN" "$BASE/api/logs?from=2026-13-99")
chk "非法日期 → 400" "[ '$code' = '400' ]"
resp=$(curl -s -H "X-Auth-Token: $TOKEN" "$BASE/api/logs?page_size=1")
n=$(echo "$resp" | python3 -c "
import sys,json
print(len(json.load(sys.stdin)['logs']))" 2>/dev/null)
chk "分页:page_size=1 只回 1 条" "[ '$n' = '1' ]"

echo "== 7. NTP 异步触发 + WebSocket 推送(ws_test.py 全流程) =="
if python3 tests/web/ws_test.py "$HOST" "$PORT" > /tmp/dg_webtest/ws.out 2>&1; then
    chk "WS:未授权 401 / 授权 101 / 不发消息也收到推送" true
else
    chk "WS:未授权 401 / 授权 101 / 不发消息也收到推送" false
    cat /tmp/dg_webtest/ws.out
fi

echo "== 8. 账号/口令修改 =="
code=$(curl -s -o /dev/null -w "%{http_code}" -X POST -H "X-Auth-Token: $TOKEN" \
    -H 'Content-Type: application/json' \
    -d '{"old_pwd":"wrong","user":"guard01","pwd":"NewPass123"}' "$BASE/api/account")
chk "旧口令错 → 400" "[ '$code' = '400' ]"
code=$(curl -s -o /dev/null -w "%{http_code}" -X POST -H "X-Auth-Token: $TOKEN" \
    -H 'Content-Type: application/json' \
    -d '{"old_pwd":"admin","user":"ab","pwd":"NewPass123"}' "$BASE/api/account")
chk "非法账号 → 400" "[ '$code' = '400' ]"
code=$(curl -s -o /dev/null -w "%{http_code}" -X POST -H "X-Auth-Token: $TOKEN" \
    -H 'Content-Type: application/json' \
    -d '{"old_pwd":"admin","user":"guard01","pwd":"bad pwd"}' "$BASE/api/account")
chk "非法口令 → 400" "[ '$code' = '400' ]"
code=$(curl -s -o /dev/null -w "%{http_code}" -X POST -H "X-Auth-Token: $TOKEN" \
    -H 'Content-Type: application/json' \
    -d '{"old_pwd":"admin","user":"guard01","pwd":"NewPass123"}' "$BASE/api/account")
chk "合法修改 → 200" "[ '$code' = '200' ]"
code=$(curl -s -o /dev/null -w "%{http_code}" -H "X-Auth-Token: $TOKEN" "$BASE/api/device")
chk "改凭据后旧 token 立即失效 → 401" "[ '$code' = '401' ]"
code=$(curl -s -o /dev/null -w "%{http_code}" -H 'Content-Type: application/json' \
    -d '{"user":"admin","pwd":"admin"}' "$BASE/api/login")
chk "旧凭据不再可登录 → 401" "[ '$code' = '401' ]"
resp=$(curl -s -H 'Content-Type: application/json' \
    -d '{"user":"guard01","pwd":"NewPass123"}' "$BASE/api/login")
TOKEN=$(echo "$resp" | jget token)
chk "新凭据可登录" "[ -n '$TOKEN' ]"
chk "改密后默认口令标记清除" "[ \"\$(echo '$resp' | jget pwd_default)\" = 'false' ]"
resp=$(curl -s -H "X-Auth-Token: $TOKEN" "$BASE/api/device")
chk "设备信息反映新账号" "[ \"\$(echo '$resp' | jget web_user)\" = 'guard01' ]"

echo "== 9. 注销 =="
code=$(curl -s -o /dev/null -w "%{http_code}" -X POST -H "X-Auth-Token: $TOKEN" "$BASE/api/logout")
chk "注销 → 200" "[ '$code' = '200' ]"
code=$(curl -s -o /dev/null -w "%{http_code}" -H "X-Auth-Token: $TOKEN" "$BASE/api/device")
chk "注销后 token 失效 → 401" "[ '$code' = '401' ]"

echo "== 10. OTA 上传闭环 =="
resp=$(curl -s -H 'Content-Type: application/json' \
    -d '{"user":"guard01","pwd":"NewPass123"}' "$BASE/api/login")
TOKEN=$(echo "$resp" | jget token)
PAYLOAD="FAKE-OTA-PAYLOAD-$(date +%s)"
SHA=$(echo -n "$PAYLOAD" | sha256sum | cut -d' ' -f1)
resp=$(curl -s -X POST -H "X-Auth-Token: $TOKEN" -H "X-OTA-Version: 9.9.9-test" \
    -H "X-OTA-Size: ${#PAYLOAD}" -H "X-OTA-SHA256: $SHA" \
    --data-binary "$PAYLOAD" "$BASE/api/ota/upload")
chk "OTA 正常包 → 200 且已暂存" "echo '$resp' | grep -q staged"
BADSHA=$(echo "$PAYLOAD-broken" | sha256sum | cut -d' ' -f1)
code=$(curl -s -o /dev/null -w "%{http_code}" -X POST -H "X-Auth-Token: $TOKEN" \
    -H "X-OTA-Version: 9.9.8" -H "X-OTA-Size: ${#PAYLOAD}" -H "X-OTA-SHA256: $BADSHA" \
    --data-binary "$PAYLOAD" "$BASE/api/ota/upload")
chk "OTA sha256 不符 → 422" "[ '$code' = '422' ]"
code=$(curl -s -o /dev/null -w "%{http_code}" -X POST -H "X-Auth-Token: $TOKEN" \
    -H "X-OTA-Version: huge" -H "X-OTA-Size: 99999999" -H "X-OTA-SHA256: $SHA" \
    --data-binary "x" "$BASE/api/ota/upload")
chk "OTA 超大小 → 400" "[ '$code' = '400' ]"

echo "== 11. mDNS:局域网按名字可解析(报文级) =="
if python3 tests/web/mdns_query_test.py "$HOST" 5353 "$(curl -s -H "X-Auth-Token: $TOKEN" "$BASE/api/device" | jget mdns_host)" "$(curl -s -H "X-Auth-Token: $TOKEN" "$BASE/api/device" | jget web_port)" \
        > /tmp/dg_webtest/mdns.out 2>&1; then
    chk "mDNS A/PTR/SRV + additional 记录正确" true
else
    chk "mDNS A/PTR/SRV + additional 记录正确" false
    cat /tmp/dg_webtest/mdns.out
fi
grep -q '不匹配名字不应答' /tmp/dg_webtest/mdns.out \
    && chk "mDNS 只答自己的名字" true || chk "mDNS 只答自己的名字" false

echo "== 12. 登录风控(放最后:会锁定来源 IP) =="
for i in 1 2 3 4; do
    code=$(curl -s -o /dev/null -w "%{http_code}" -H 'Content-Type: application/json' \
        -d '{"user":"guard01","pwd":"wrong"}' "$BASE/api/login")
    chk "第 $i 次错误 → 401 且未锁" "[ '$code' = '401' ]"
done
code=$(curl -s -o /dev/null -w "%{http_code}" -H 'Content-Type: application/json' \
    -d '{"user":"guard01","pwd":"wrong"}' "$BASE/api/login")
chk "第 5 次错误触发锁定(401/429)" "[ '$code' = '401' ] || [ '$code' = '429' ]"
code=$(curl -s -o /dev/null -w "%{http_code}" -H 'Content-Type: application/json' \
    -d '{"user":"guard01","pwd":"NewPass123"}' "$BASE/api/login")
chk "锁定期内口令正确也拒 → 429" "[ '$code' = '429' ]"

kill $SRV_PID 2>/dev/null
echo
echo "web 验收: $PASS 通过, $FAIL 失败"
[ "$FAIL" = 0 ]
