#!/usr/bin/env bash
#
# NovaServer 功能冒烟
#
# 走通静态资源、URL 转义、HEAD、错误状态码、注册、登录、上传闭环，
# 并验证空闲连接会按预期被回收。用于改动后确认主链路未被破坏——
# 压测脚本只看吞吐，功能是否还完整它答不了。
#
# 另含 `033` 全项目审核所修缺陷的回归用例（绝对形式 URI、管线化、启动参数校验），
# 这几条修复此前只在当时手工验证过一次，纳入本脚本后才能防止复发。
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
# 启动参数校验用例使用的端口。须给一个空闲端口，否则「越界参数未被拒绝」时
# 服务端会因端口被占而退出，看起来像被拒绝了——那是假通过。
REGRESS_PORT="${REGRESS_PORT:-9299}"

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

echo "=== 缺陷回归（033 修复项）==="
# A1（绝对形式 URI 且无路径 → 空指针解引用）与 A2（管线化请求被静默丢弃）都继承自原项目，
# 且都曾造成可观测故障：前者一条请求即可打挂单进程服务端。两者都需要原始套接字——
# curl 会把请求行规范化，测不出绝对形式；故用 python 直发。
if python3 - "$HOST" "$PORT" <<'PY'
import socket, sys

host, port = sys.argv[1], int(sys.argv[2])

def raw(payload, timeout=3.0):
    s = socket.create_connection((host, port), timeout=timeout)
    s.settimeout(timeout)
    s.sendall(payload)
    buf = b""
    try:
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
    except socket.timeout:
        pass
    s.close()
    return buf

def status(buf):
    return buf.split(b"\r\n", 1)[0].decode(errors="replace")

fails = 0

def check(name, cond, detail):
    global fails
    print("  %s  %s — %s" % ("通过" if cond else "失败", name, detail))
    if not cond:
        fails += 1

r = raw(b"GET http://example.com HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n")
check("绝对形式 URI 无路径", b"400" in r.split(b"\r\n")[0], status(r))

r = raw(b"GET http://example.com/judge.html HTTP/1.1\r\nHost: example.com\r\n"
        b"Connection: close\r\n\r\n")
check("带路径的绝对形式仍正常", b"200" in r.split(b"\r\n")[0], status(r))

r = raw(b"GET /judge.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
check("上述请求后服务端存活", b"200" in r.split(b"\r\n")[0], status(r))

r = raw(b"GET /judge.html HTTP/1.1\r\nHost: x\r\n\r\n"
        b"GET /judge.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
n = r.count(b"HTTP/1.1 200")
check("管线化两个请求", n == 2, "收到 %d 个 200 响应" % n)

sys.exit(1 if fails else 0)
PY
then ok "缺陷回归：绝对形式 URI 与管线化"; else bad "缺陷回归：绝对形式 URI 与管线化"; fi

# 启动参数校验：越界值必须在绑定端口之前被拒绝。这些用例不依赖上面那个服务端，
# 而是直接拉起二进制；若二进制不在预期位置则跳过。
SERVER_BIN="$REPO_ROOT/server"
if [ -x "$SERVER_BIN" ]; then
  for spec in "连接数为 0:-p ${REGRESS_PORT} -s 0" "端口越界:-p 74542" "关闭方式越界:-p ${REGRESS_PORT} -o 5"; do
    desc="${spec%%:*}"; args="${spec#*:}"
    # RC=124 表示超时，即服务端没有被拒绝而是启动成功了——属失败
    out=$(timeout 5 "$SERVER_BIN" $args 2>&1); rc=$?
    if [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ]; then
      ok "启动参数校验：$desc 被拒绝（退出码 $rc）"
    else
      bad "启动参数校验：$desc 未被拒绝（退出码 $rc）"
    fi
  done
else
  echo "  已跳过启动参数校验（未找到可执行的 $SERVER_BIN）"
fi

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
