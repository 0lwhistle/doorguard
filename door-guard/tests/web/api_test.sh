#!/usr/bin/env bash
# api_test.sh — web 上位机 REST API 功能验收(用户管理/门禁设置/鉴权)
#
# 前置:一个运行中的 door-guard(宿主 sim 或板上),且库里没有 uid=
# webtest1 的用户(脚本会增删它)。板上跑把 B 改成 http://<板IP>:8080。
# 用法:bash tests/web/api_test.sh   (退出码 0 = 全过)
# 注意:全部断言用固定字符串(grep -F),cJSON 输出是紧凑无空格格式。
# web_api_test.sh — 宿主 sim 上对新增 web API 的功能验收
set -u
B=http://127.0.0.1:8080
PASS=0; FAIL=0
ck() { # ck <描述> <期望子串> <实际响应>
  if echo "$3" | grep -qF -- "$2"; then PASS=$((PASS+1)); echo "ok   $1";
  else FAIL=$((FAIL+1)); echo "FAIL $1: 期望含[$2] 实际[$3]"; fi
}
jq_get() { python3 -c "import sys,json;d=json.load(sys.stdin);print(d$1)" 2>/dev/null; }

# 未授权 → 401
R=$(curl -s -m 3 $B/api/users)
ck "未授权列表被拒" "未登录" "$R"

# 登录
TOK=$(curl -s -m 3 -X POST $B/api/login -H "Content-Type: application/json" \
  -d '{"user":"admin","pwd":"admin"}' | jq_get "['token']")
A="X-Auth-Token: $TOK"
[ -n "$TOK" ] && { PASS=$((PASS+1)); echo "ok   登录"; } || { FAIL=$((FAIL+1)); echo "FAIL 登录"; }

# 初始列表
R=$(curl -s -m 3 -H "$A" "$B/api/users")
ck "列表结构完整" '"users":[' "$R"

# 添加合法用户
R=$(curl -s -m 3 -X POST $B/api/users/add -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest1","name":"网页用户","pwd":"test1234","role":0,"auth_flags":5}')
ck "添加合法用户" "已添加" "$R"

# 重复 uid
R=$(curl -s -m 3 -X POST $B/api/users/add -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest1","name":"x","pwd":"test1234"}')
ck "重复 uid 被拒" "该用户ID已存在" "$R"

# 非法 uid(短)
R=$(curl -s -m 3 -X POST $B/api/users/add -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"ab","name":"x","pwd":"test1234"}')
ck "过短 uid 被拒" "ID 需 3~31" "$R"

# 非法密码(含空格)
R=$(curl -s -m 3 -X POST $B/api/users/add -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest2","name":"x","pwd":"a b c d"}')
ck "含空格密码被拒" "密码 4~31" "$R"

# 缺密码
R=$(curl -s -m 3 -X POST $B/api/users/add -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest3","name":"x"}')
ck "缺密码被拒" "缺少必填" "$R"

# role 越界
R=$(curl -s -m 3 -X POST $B/api/users/add -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest4","name":"x","pwd":"test1234","role":7}')
ck "role 越界被拒" "缺少必填" "$R"

# 列表含新用户
R=$(curl -s -m 3 -H "$A" "$B/api/users?page=1&page_size=50")
ck "列表含 webtest1" "webtest1" "$R"
ck "列表含姓名" "网页用户" "$R"

# 编辑:改权限为管理员
R=$(curl -s -m 3 -X POST $B/api/users/update -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest1","role":1}')
ck "编辑权限" "已保存" "$R"
R=$(curl -s -m 3 -H "$A" "$B/api/users?page=1&page_size=50")
ck "权限已生效" '"role":1' "$R"

# 编辑:非法 auth_flags=0
R=$(curl -s -m 3 -X POST $B/api/users/update -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest1","auth_flags":0}')
ck "auth_flags=0 被拒" "1~15" "$R"

# 改密
R=$(curl -s -m 3 -X POST $B/api/users/pwd -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest1","pwd":"newpass99"}')
ck "重置密码" "密码已更新" "$R"

# 改密:弱密码
R=$(curl -s -m 3 -X POST $B/api/users/pwd -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest1","pwd":"123"}')
ck "弱密码被拒" "密码 4~31" "$R"

# 删除(受理制)
R=$(curl -s -m 3 -X POST $B/api/users/delete -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest1"}')
ck "删除受理" "已受理" "$R"
sleep 1
R=$(curl -s -m 3 -H "$A" "$B/api/users?page=1&page_size=50")
if echo "$R" | grep -q webtest1; then FAIL=$((FAIL+1)); echo "FAIL 删除后仍在列表"; else PASS=$((PASS+1)); echo "ok   删除后列表无此人"; fi

# 删除不存在
R=$(curl -s -m 3 -X POST $B/api/users/delete -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"nouser99"}')
ck "删不存在回 404 文案" "用户不存在" "$R"

# 远程重启(宿主 sim 是 DG_SYSCTL_FAKE 模拟执行,不会真重启)
R=$(curl -s -m 3 $B/api/system/reboot)
ck "重启只接受 POST" "重启接口只接受 POST" "$R"
R=$(curl -s -m 3 -X POST $B/api/system/reboot)
ck "未授权重启被拒" "未登录" "$R"
R=$(curl -s -m 3 -X POST $B/api/system/reboot -H "$A")
ck "重启请求受理" "重启请求已受理" "$R"
sleep 2
R=$(curl -s -m 3 -H "$A" "$B/api/device")
ck "模拟重启后设备仍在(宿主不真重启)" '"version"' "$R"

# 门禁设置读
R=$(curl -s -m 3 -H "$A" "$B/api/access_set")
ck "设置含 door_open_ms" "door_open_ms" "$R"
ck "设置含阈值" "face_match_threshold" "$R"
ck "设置含范围" '"min":1000' "$R"

# 门禁设置写(合法)
R=$(curl -s -m 3 -X POST $B/api/access_set -H "$A" -H "Content-Type: application/json" \
  -d '{"door_open_ms":5000,"face_match_threshold":0.45}')
ck "设置写入" "已保存" "$R"
R=$(curl -s -m 3 -H "$A" "$B/api/access_set")
ck "door_open_ms=5000 生效" '"value":5000' "$R"
ck "阈值 0.45 生效" '"value":0.45' "$R"

# 门禁设置写(越界整批拒绝)
R=$(curl -s -m 3 -X POST $B/api/access_set -H "$A" -H "Content-Type: application/json" \
  -d '{"door_open_ms":99}')
ck "越界整批拒绝" "需在" "$R"
R=$(curl -s -m 3 -H "$A" "$B/api/access_set")
ck "越界未生效(仍 5000)" '"value":5000' "$R"

# 未知设置项
R=$(curl -s -m 3 -X POST $B/api/access_set -H "$A" -H "Content-Type: application/json" \
  -d '{"hacker_key":1}')
ck "未知项拒绝" "未知设置项" "$R"

# 恢复默认值
curl -s -m 3 -X POST $B/api/access_set -H "$A" -H "Content-Type: application/json" \
  -d '{"door_open_ms":3000,"face_match_threshold":0.42}' > /dev/null

echo "=============================="
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" = 0 ]
