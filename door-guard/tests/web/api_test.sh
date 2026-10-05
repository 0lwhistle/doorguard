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

# 登录
TOK=$(curl -s -m 3 -X POST $B/api/login -H "Content-Type: application/json" \
  -d '{"user":"admin","pwd":"admin"}' | jq_get "['token']")
A="X-Auth-Token: $TOK"
[ -n "$TOK" ] && { PASS=$((PASS+1)); echo "ok   登录"; } || { FAIL=$((FAIL+1)); echo "FAIL 登录"; }

# 初始列表
R=$(curl -s -m 3 -H "$A" "$B/api/users")
ck "列表结构完整" '"users":[' "$R"

# 添加合法用户(方式位只带密码:不变式下新用户没有已录凭据)
R=$(curl -s -m 3 -X POST $B/api/users/add -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest1","name":"网页用户","pwd":"test1234","role":0,"auth_flags":4}')
ck "添加合法用户" "已添加" "$R"

# 建号带人脸位(无凭据)→ 不变式拒收
R=$(curl -s -m 3 -X POST $B/api/users/add -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest9","name":"x","pwd":"test1234","auth_flags":5}')
ck "建号带人脸位被拒" "该验证方式未录入" "$R"

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

# 方式位不变式:未录入人脸不允许开启人脸位
R=$(curl -s -m 3 -X POST $B/api/users/update -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest1","auth_flags":5}')
ck "未录人脸开人脸位被拒" "该验证方式未录入" "$R"

# ---- 人脸录入/重录(受理制;2026-10-04)----
FACE_JPG=/tmp/dg_api_face_$$.jpg
printf '\xff\xd8\xff\xe0\x00\x10JFIF\x00' > "$FACE_JPG"   # SOI 形态(sim 后端不解析内容)
head -c 2048 /dev/zero >> "$FACE_JPG"

R=$(curl -s -m 3 -X POST "$B/api/users/face_set?uid=webtest1" -H "$A" --data-binary '')
ck "人脸录入空体被拒" "图片大小" "$R"
R=$(curl -s -m 3 -X POST "$B/api/users/face_set?uid=webtest1" -H "$A" --data-binary 'notjpeg')
ck "非 JPEG 被拒" "仅支持 JPEG" "$R"
R=$(curl -s -m 3 -X POST "$B/api/users/face_set?uid=nouser99" -H "$A" --data-binary @"$FACE_JPG")
ck "不存在用户回 404 文案" "用户不存在" "$R"
R=$(curl -s -m 3 -X POST "$B/api/users/face_set?uid=webtest1" -H "$A" -H "Content-Type: image/jpeg" \
  --data-binary @"$FACE_JPG")
ck "人脸录入受理" '"seq"' "$R"
sleep 1                                                    # 异步提取落库
R=$(curl -s -m 3 -H "$A" "$B/api/users?page=1&page_size=50")
ck "录入后列表 has_face" '"has_face":true' "$R"

# 录入完成后人脸位可开(重录语义同路径)
R=$(curl -s -m 3 -X POST $B/api/users/update -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest1","auth_flags":5}')
ck "已录人脸开人脸位通过" "已保存" "$R"

# 清人脸(受理制):方式位自动回收
R=$(curl -s -m 3 -X POST $B/api/users/face_clear -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest1"}')
ck "清人脸受理" "已受理" "$R"
sleep 1
R=$(curl -s -m 3 -H "$A" "$B/api/users?page=1&page_size=50")
ck "清脸后 has_face=false" '"has_face":false' "$R"
rm -f "$FACE_JPG"

# ---- IC 卡绑定/解绑(卡号直输同步落库;2026-10-05)----
# 未录 IC 时方式位不变式拦 IC 位(密码|IC = 4|8 = 12)
R=$(curl -s -m 3 -X POST $B/api/users/update -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest1","auth_flags":12}')
ck "未绑卡开 IC 位被拒" "该验证方式未录入" "$R"

# 缺字段/格式非法
R=$(curl -s -m 3 -X POST $B/api/users/ic_set -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest1"}')
ck "绑卡缺 card_no 被拒" "需要 uid 与 card_no" "$R"
R=$(curl -s -m 3 -X POST $B/api/users/ic_set -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest1","card_no":"04a3b2"}')
ck "卡号 3 字节被拒" "8~30 位十六进制" "$R"
R=$(curl -s -m 3 -X POST $B/api/users/ic_set -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest1","card_no":"04A3B2G1"}')
ck "卡号非 HEX 被拒" "8~30 位十六进制" "$R"
R=$(curl -s -m 3 -X POST $B/api/users/ic_set -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"nouser99","card_no":"04A3B2C1"}')
ck "给不存在用户绑卡回 404 文案" "用户不存在" "$R"

# 小写归一 + 绑定成功,列表回掩码(原卡号不出设备)
R=$(curl -s -m 3 -X POST $B/api/users/ic_set -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest1","card_no":"04a3b2c1"}')
ck "绑卡成功(小写归一)" "IC 卡已绑定" "$R"
ck "绑卡回执带掩码" '"ic_mask":"********B2C1"' "$R"
R=$(curl -s -m 3 -H "$A" "$B/api/users?page=1&page_size=50")
ck "列表 has_ic=true" '"has_ic":true' "$R"
ck "列表掩码展示" '"ic_mask":"********B2C1"' "$R"

# 方式位不变式:已绑卡后 IC 位可开
R=$(curl -s -m 3 -X POST $B/api/users/update -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest1","auth_flags":12}')
ck "已绑卡开 IC 位通过" "已保存" "$R"

# 重复绑卡:第二用户绑同卡被拒
R=$(curl -s -m 3 -X POST $B/api/users/add -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest6","name":"六号","pwd":"test1234","role":0,"auth_flags":4}')
ck "添加第二个用户" "已添加" "$R"
R=$(curl -s -m 3 -X POST $B/api/users/ic_set -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest6","card_no":"04A3B2C1"}')
ck "同卡绑第二人被拒" "该卡已绑定其他用户" "$R"

# 解绑:清卡 + 方式位回收
R=$(curl -s -m 3 -X POST $B/api/users/ic_clear -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest1"}')
ck "解绑成功" "IC 卡已解绑" "$R"
R=$(curl -s -m 3 -H "$A" "$B/api/users?page=1&page_size=50")
ck "解绑后 has_ic=false" '"has_ic":false' "$R"
R=$(curl -s -m 3 -X POST $B/api/users/ic_clear -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest1"}')
ck "重复解绑幂等" "IC 卡已解绑" "$R"
R=$(curl -s -m 3 -X POST $B/api/users/ic_clear -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"nouser99"}')
ck "解绑不存在用户回 404 文案" "用户不存在" "$R"

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

# 清理 IC 段创建的第二个用户(重跑脚本不因残留 uid 挂掉)
curl -s -m 3 -X POST $B/api/users/delete -H "$A" -H "Content-Type: application/json" \
  -d '{"uid":"webtest6"}' > /dev/null
sleep 1

# ---- 固件升级包槽(.ota 全量包;2026-10-05) ----
# 构造最小合法 .ota(96B 头 + 4B 载荷;摘要随包计算)。用独立脚本文件而非
# 内联 heredoc:转义经多层(shell→python)极易失真,NUL 混入会让 git 把
# 本脚本当二进制(实测坑)。
cat > /tmp/mk_ota.py <<'PYPKG'
import hashlib, struct
payload = bytes([0xAA, 0xBB, 0xCC, 0xDD])
sha = hashlib.sha256(payload).digest()
hdr = bytearray(96)
hdr[0:8] = b'DGOTA1' + bytes(2)
struct.pack_into('>I', hdr, 8, 96)
struct.pack_into('>I', hdr, 12, len(payload))
hdr[16:21] = b'0.0.1'
hdr[64:96] = sha
open('/tmp/apitest.ota', 'wb').write(bytes(hdr) + payload)
PYPKG
python3 /tmp/mk_ota.py

# 未授权 → 401
R=$(curl -s -m 3 $B/api/users)
ck "未授权列表被拒" "未登录" "$R"

# 升级包:未授权上传被拒
R=$(curl -s -m 5 -X POST $B/api/ota/fw/upload -H "X-OTA-Size: 100" --data-binary @/tmp/apitest.ota)
ck "升级包未授权上传被拒" "未登录" "$R"

# 升级包:缺 X-OTA-Size 被拒
R=$(curl -s -m 5 -X POST $B/api/ota/fw/upload -H "$A" --data-binary @/tmp/apitest.ota)
ck "升级包缺 Size 头被拒" "缺少 X-OTA-Size" "$R"

# 升级包:好包入槽
R=$(curl -s -m 10 -X POST $B/api/ota/fw/upload -H "$A" \
    -H "X-OTA-Size: $(stat -c %s /tmp/apitest.ota)" --data-binary @/tmp/apitest.ota)
ck "升级包入槽" '"version":"0.0.1"' "$R"

# 升级包:状态可见
R=$(curl -s -m 3 -H "$A" $B/api/ota/fw)
ck "升级包状态含版本" '"version":"0.0.1"' "$R"
ck "升级包状态含暂存标志" '"staged":' "$R"

# 升级包:坏包(截断)→ 422
head -c 98 /tmp/apitest.ota > /tmp/apitest_bad.ota   # 96B<98<100:过前置大小检、砸中「总长与头不符」
R=$(curl -s -m 5 -X POST $B/api/ota/fw/upload -H "$A" \
    -H "X-OTA-Size: $(stat -c %s /tmp/apitest_bad.ota)" --data-binary @/tmp/apitest_bad.ota)
ck "坏包 422 拒收" "包校验失败" "$R"

# 升级包:空槽 apply → 409(真升级板上人工验收,不进自动化)
curl -s -m 3 -X DELETE $B/api/ota/fw -H "$A" >/dev/null
R=$(curl -s -m 3 -X POST $B/api/ota/fw/apply -H "$A")
ck "空槽 apply 被拒" "槽内没有升级包" "$R"

# 升级包:删除后再查为空
R=$(curl -s -m 3 -H "$A" $B/api/ota/fw)
ck "删除后槽为空" '"present":false' "$R"

echo "=============================="
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" = 0 ]
