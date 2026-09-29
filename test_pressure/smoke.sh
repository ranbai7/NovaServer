#!/usr/bin/env bash
#
# NovaServer 功能冒烟
#
# 走通静态资源、URL 转义、HEAD、错误状态码、注册、登录、上传闭环，
# 并验证空闲连接会按预期被回收。用于改动后确认主链路未被破坏——
# 压测脚本只看吞吐，功能是否还完整它答不了。
#
# 用法:
#   ./smoke.sh [端口]        默认 9006，需先把服务端跑在该端口上
#
# 环境变量:
#   HOST=127.0.0.1        服务地址
#   IDLE_WAIT=17          超时回收用例的等待秒数（须大于服务端的空闲回收时间）
#   SMOKE_DB=0            是否验证登录/注册，默认开启。该组用例会往数据库里写入
#                         一个以进程号命名的一次性用户（不会自动清理）
#
# 退出码非 0 表示有用例未通过。
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${HOST:-127.0.0.1}"
PORT="${1:-9006}"
BASE="http://${HOST}:${PORT}"
IDLE_WAIT="${IDLE_WAIT:-17}"
SMOKE_DB="${SMOKE_DB:-1}"

PASS=0
FAIL=0

ok()   { PASS=$((PASS + 1)); printf '  \033[32m通过\033[0m  %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  \033[31m失败\033[0m  %s\n' "$1"; }

# check <说明> <期望状态码> <curl 参数...>
check_code() {
  local desc="$1" want="$2"; shift 2
  local got
  got=$(curl -s -o /dev/null -w '%{http_code}' "$@" 2>/dev/null)
  if [ "$got" = "$want" ]; then ok "$desc（$got）"; else bad "$desc（期望 $want，实际 $got）"; fi
}

# 等待服务端就绪
for _ in $(seq 1 60); do
  curl -s -o /dev/null "${BASE}/judge.html" 2>/dev/null && break
  sleep 0.2
done

echo "=== 静态资源与协议 ==="
check_code "GET 静态页面"          200 "${BASE}/judge.html"
check_code "GET 站点根"            200 "${BASE}/"
check_code "GET 不存在的页面"      404 "${BASE}/no-such-page.html"
check_code "GET URL 转义路径"      200 "${BASE}/%6audge.html"
check_code "HEAD 静态页面"         200 -I "${BASE}/judge.html"
check_code "GET 路径穿越"          400 --path-as-is "${BASE}/../config.ini"

# 响应头形如 Content-Length:2316，冒号后没有空格
BODY=$(curl -s -I "${BASE}/judge.html" 2>/dev/null | tr -d '\r' | awk -F: '/^Content-Length/{print $2}')
if [ -n "$BODY" ]; then ok "HEAD 返回 Content-Length（$BODY）"; else bad "HEAD 未返回 Content-Length"; fi

echo "=== 注册与登录 ==="
if [ "$SMOKE_DB" = "1" ]; then
  USER="smoke_$$"
  PASSWD="pw_$$_secret"
  check_code "POST 注册新用户" 200 -X POST -d "user=${USER}&password=${PASSWD}" "${BASE}/3CGISQL.cgi"
  check_code "POST 同名重复注册" 200 -X POST -d "user=${USER}&password=${PASSWD}" "${BASE}/3CGISQL.cgi"
  check_code "POST 登录"       200 -X POST -d "user=${USER}&password=${PASSWD}" "${BASE}/2CGISQL.cgi"
  check_code "POST 口令错误"   200 -X POST -d "user=${USER}&password=wrong" "${BASE}/2CGISQL.cgi"
  check_code "POST 缺少字段"   400 -X POST -d "user=${USER}" "${BASE}/2CGISQL.cgi"
else
  echo "  已跳过（SMOKE_DB=0）"
fi

echo "=== 文件上传闭环 ==="
TMP=$(mktemp -d)
printf 'novaserver smoke payload\n' > "$TMP/novasmoke_$$.txt"
printf '<html>not allowed</html>\n' > "$TMP/novasmoke_$$.html"

check_code "POST 上传允许的类型" 200 -X POST -F "file=@$TMP/novasmoke_$$.txt" "${BASE}/upload"
check_code "POST 上传禁止的类型" 415 -X POST -F "file=@$TMP/novasmoke_$$.html" "${BASE}/upload"
check_code "GET 上传列表"        200 "${BASE}/upload"

LIST=$(curl -s "${BASE}/upload" 2>/dev/null)
if grep -q "novasmoke_$$.txt" <<<"$LIST"; then ok "上传列表含新文件"; else bad "上传列表不含新文件"; fi

GOT=$(curl -s "${BASE}/upload/novasmoke_$$.txt" 2>/dev/null)
if grep -q "novaserver smoke payload" <<<"$GOT"; then ok "取回上传的文件内容一致"; else bad "取回的上传文件内容不符"; fi

rm -rf "$TMP"

echo "=== 空闲连接回收 ==="
if python3 - "$HOST" "$PORT" "$IDLE_WAIT" <<'PY'
import socket, sys, time
host, port, wait = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
s = socket.create_connection((host, port), timeout=5)
s.settimeout(5)
time.sleep(wait)                      # 期间不发任何数据
try:
    data = s.recv(1)
except socket.timeout:
    print("    连接在 %s 秒后仍然存活" % wait)
    sys.exit(1)
if data == b"":
    sys.exit(0)                       # 对端已关闭，符合预期
print("    收到了意外数据")
sys.exit(1)
PY
then ok "空闲连接在 ${IDLE_WAIT} 秒内被回收"; else bad "空闲连接未被回收"; fi

echo
echo "通过 $PASS 项，失败 $FAIL 项"
[ "$FAIL" -eq 0 ]
