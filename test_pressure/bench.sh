#!/usr/bin/env bash
#
# NovaServer 压测脚本
#
# 在同一环境下依次运行多组服务端配置，每组重复多次并输出中位数，
# 用于建立性能基线或验证优化效果。
#
# 用法:
#   ./bench.sh                    使用默认参数
#   ./bench.sh 100 5s 5           并发数 时长 重复次数
#
# 环境变量:
#   PORT=9006        服务端口
#   WRK=wrk          压测工具路径
#   URL_PATH=/judge.html   压测路径
#   RESULT_DIR=...   结果输出目录
#
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-9006}"
WRK_BIN="${WRK:-wrk}"
URL_PATH="${URL_PATH:-/judge.html}"
URL="http://127.0.0.1:${PORT}${URL_PATH}"

CONCURRENCY="${1:-100}"
DURATION="${2:-5s}"
REPEATS="${3:-5}"
THREADS="${THREADS:-2}"

RESULT_DIR="${RESULT_DIR:-$REPO_ROOT/test_pressure/results}"
mkdir -p "$RESULT_DIR"
RAW_FILE="$RESULT_DIR/raw_$(date +%Y%m%d_%H%M%S).txt"

# 参数组合: "<触发模式> <并发模型> <线程数>"，对应 ./server -m -a -t
COMBOS=(
  "0 0 8"
  "1 0 8"
  "2 0 8"
  "3 0 8"
  "0 1 8"
  "1 1 8"
  "2 1 8"
  "3 1 8"
  "0 0 1"
  "0 0 2"
  "0 0 4"
)

SUMMARY=()
SERVER_PID=""

log() { echo "$*" | tee -a "$RAW_FILE"; }

# 将 wrk 输出的延迟统一换算为毫秒，便于排序比较
to_ms() {
  case "$1" in
    *us) sed 's/us//' <<<"$1" | awk '{printf "%.3f", $1/1000}' ;;
    *ms) sed 's/ms//' <<<"$1" ;;
    *s)  sed 's/s//'  <<<"$1" | awk '{printf "%.3f", $1*1000}' ;;
    *)   echo "$1" ;;
  esac
}

median() {
  printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END{printf "%.3f", a[int((NR+1)/2)]}'
}

environment_info() {
  log "=== 测试环境 ==="
  log "时间       : $(date '+%Y-%m-%d %H:%M:%S')"
  log "主机       : $(uname -n)"
  log "内核       : $(uname -r)"
  log "虚拟化     : $(systemd-detect-virt 2>/dev/null || echo unknown)"
  log "CPU        : $(lscpu | grep -m1 '型号名称\|Model name' | sed 's/.*: *//')"
  log "CPU 核数   : $(nproc)"
  log "内存总量   : $(free -h | awk '/内存|^Mem/{print $2}')"
  log "服务端     : $(ls -l "$REPO_ROOT/server" 2>/dev/null | awk '{print $5" bytes  "$6" "$7" "$8}' || echo '未编译')"
  log "wrk        : $("$WRK_BIN" --version 2>&1 | head -1)"
  log "压测目标   : $URL"
  log "压测参数   : -c$CONCURRENCY -d$DURATION (重复 $REPEATS 次，wrk $THREADS 线程)"
  log ""
}

start_server() {
  (cd "$REPO_ROOT" && exec ./server -p "$PORT" -m "$1" -a "$2" -t "$3" -c 1) >/dev/null 2>&1 &
  SERVER_PID=$!
  for _ in $(seq 1 50); do
    if ss -lnt 2>/dev/null | grep -q ":${PORT} "; then return 0; fi
    sleep 0.1
  done
  return 1
}

stop_server() {
  [ -n "$SERVER_PID" ] || return 0
  kill "$SERVER_PID" 2>/dev/null
  wait "$SERVER_PID" 2>/dev/null
  SERVER_PID=""
}

run_group() {
  local m=$1 a=$2 t=$3
  local label="-m $m -a $a -t $t"
  local qps=() p50=() p99=() rss=""

  log "### $label"

  for i in $(seq 1 "$REPEATS"); do
    if ! start_server "$m" "$a" "$t"; then
      log "  启动失败，跳过"
      stop_server
      return 1
    fi

    [ -n "$rss" ] || rss=$(ps -o rss= -p "$SERVER_PID" | awk '{printf "%.1f", $1/1024}')

    local out
    out=$("$WRK_BIN" -t"$THREADS" -c"$CONCURRENCY" -d"$DURATION" --latency "$URL" 2>/dev/null)
    stop_server

    local q
    q=$(awk '/Requests\/sec/{printf "%.0f", $2}' <<<"$out")
    local a50 a99
    a50=$(to_ms "$(awk '/^ *50%/{print $2}' <<<"$out")")
    a99=$(to_ms "$(awk '/^ *99%/{print $2}' <<<"$out")")

    qps+=("$q"); p50+=("$a50"); p99+=("$a99")
    log "  第 $i 次: QPS=$q  P50=${a50}ms  P99=${a99}ms"
  done

  local mq m50 m99
  mq=$(median "${qps[@]}")
  m50=$(median "${p50[@]}")
  m99=$(median "${p99[@]}")

  log "  -- 中位数: QPS=$mq  P50=${m50}ms  P99=${m99}ms  RSS=${rss}MB"
  log ""
  SUMMARY+=("| $m | $a | $t | $mq | $m50 | $m99 | $rss |")
}

main() {
  environment_info

  for combo in "${COMBOS[@]}"; do
    # shellcheck disable=SC2086
    run_group $combo
  done

  log "=== 汇总（中位数）==="
  log "| 触发模式 | 并发模型 | 线程数 | QPS | P50 (ms) | P99 (ms) | RSS (MB) |"
  log "|:--:|:--:|--:|--:|--:|--:|--:|"
  for row in "${SUMMARY[@]}"; do log "$row"; done
  log ""
  log "原始输出已保存至: $RAW_FILE"
}

main
