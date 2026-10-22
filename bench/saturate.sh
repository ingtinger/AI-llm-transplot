#!/usr/bin/env bash

# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

# Find the single-thread saturation point of the llmbridge proxy.
#
# Uses the C++ fastbackend (instant response) so the *backend* never saturates
# first, confirmed by also driving the backend directly at each RPS. The
# saturation point is the highest target RPS where the proxy still tracks the
# target (achieved ~= target) with a tight client tail; past it, achieved
# plateaus, the loadgen backlog explodes, and client p99 climbs while the
# direct-to-backend leg keeps tracking, proving the proxy is the bottleneck.
#
#   ./bench/saturate.sh [dur] [warmup] [rps_list...]
set -uo pipefail
cd "$(dirname "$0")/.."

DUR="${1:-6}"
WARMUP="${2:-2}"
shift $(( $# < 2 ? $# : 2 )) || true
RPS_LIST=("${@:-20000 40000 60000 80000 100000 120000}")
if [ "${#RPS_LIST[@]}" -eq 1 ]; then read -r -a RPS_LIST <<< "${RPS_LIST[0]}"; fi
BODY="${BODY:-64}" # request body size in bytes (loadgen --body-bytes)
WORKERS="${WORKERS:-1}" # number of SO_REUSEPORT worker threads
IO="${IO:-uring}"        # event-loop backend (auto|epoll|uring)
BACKENDS="${BACKENDS:-1}" # mock backend instances (SO_REUSEPORT) so the backend is not the cap

BACK_PORT=9002
PROXY_PORT=8090
CONNS=2048
RESDIR=bench/results
mkdir -p "$RESDIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$RESDIR/saturation-$STAMP.txt"

# Locate CMake-built binaries (CLion uses cmake-build-*/bin). Override: BIN=path
BIN="${BIN:-}"
if [ -z "$BIN" ]; then
  for d in cmake-build-release/bin cmake-build-debug/bin build-linux/bin build/bin; do
    if [ -x "$d/llmbridge" ]; then BIN="$d"; break; fi
  done
fi
[ -z "$BIN" ] && { echo "No built binaries; build the CMake project first or set BIN=path" >&2; exit 1; }

BACK_PIDS=()
PP=""
cleanup() {
  for pid in "$PP" "${BACK_PIDS[@]}"; do
    if [ -n "$pid" ]; then
      kill -TERM "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT
echo "Using binaries in $BIN"

for b in $(seq 1 "$BACKENDS"); do
  $BIN/fastbackend --port "$BACK_PORT" --latency-us 0 >>"$RESDIR/fb-$STAMP.log" 2>&1 &
  BACK_PIDS+=("$!")
done
sleep 0.5
for pid in "${BACK_PIDS[@]}"; do
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "Mock 启动失败或端口 $BACK_PORT 已占用" >&2
    exit 1
  fi
done

extract() { sed -n 's/.*achieved=\([0-9]*\) .*p50_us=\([0-9]*\) p99_us=\([0-9]*\) p999_us=\([0-9]*\) max_us=\([0-9]*\).*/\1 \2 \3 \4 \5/p'; }
extract_counts() { sed -n 's/.*completed=\([0-9]*\) errors=\([0-9]*\) .*/\1 \2/p'; }
backlog_of() { sed -n 's/.*max_backlog=\([0-9]*\).*/\1/p' "$1" | tail -1; }
cpu_ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }
peak_rss_kb() { awk '$1 == "VmHWM:" {print $2}' "/proc/$1/status" 2>/dev/null || echo 0; }
ticks_per_s=$(getconf CLK_TCK)

{
  echo "llmbridge single-thread saturation, $STAMP  (instant C++ backend, ${DUR}s/level, ${WARMUP}s warmup, body=${BODY}B, io=${IO}, workers=${WORKERS}, backends=${BACKENDS})"
  printf "%-9s | %-22s | %-38s | %s\n" "TARGET" "DIRECT backend" "THROUGH proxy" "proxy"
  printf "%-9s | %-22s | %-38s | %s\n" "RPS" "achieved (p99 µs)" "achieved / p50 / p99 / p99.9 / max (µs)" "backlog"
  echo "----------+------------------------+----------------------------------------+--------"
} | tee "$OUT"

for RPS in "${RPS_LIST[@]}"; do
  echo ">>> RPS=$RPS" >&2
  # direct-to-backend control
  D=$($BIN/loadgen --target 127.0.0.1:$BACK_PORT --rps "$RPS" --duration "$DUR" --warmup "$WARMUP" --conns "$CONNS" --body-bytes "$BODY" 2>/dev/null)
  read -r da d_p50 d_p99 d_p999 d_max <<< "$(echo "$D" | extract)"
  read -r d_completed d_errors <<< "$(echo "$D" | extract_counts)"

  # fresh proxy per level
  $BIN/llmbridge --legacy-mode --listen $PROXY_PORT --upstream 127.0.0.1:$BACK_PORT --io "$IO" --workers "$WORKERS" \
      --duration $((DUR + WARMUP + 3)) --warmup "$WARMUP" >"$RESDIR/sat-proxy-$RPS-$STAMP.log" 2>&1 &
  PP=$!
  sleep 0.7
  if ! kill -0 "$PP" 2>/dev/null; then
    echo "网关启动失败或端口 $PROXY_PORT 已占用" >&2
    exit 1
  fi
  proxy_cpu_start=$(cpu_ticks "$PP")
  PLOG="$RESDIR/sat-loadgen-$RPS-$STAMP.log"
  P=$($BIN/loadgen --target 127.0.0.1:$PROXY_PORT --rps "$RPS" --duration "$DUR" --warmup "$WARMUP" --conns "$CONNS" --body-bytes "$BODY" 2>"$PLOG")
  proxy_cpu_end=$(cpu_ticks "$PP")
  proxy_rss=$(peak_rss_kb "$PP")
  read -r pa p_p50 p_p99 p_p999 p_max <<< "$(echo "$P" | extract)"
  read -r p_completed p_errors <<< "$(echo "$P" | extract_counts)"
  bl=$(backlog_of "$PLOG")
  kill -TERM "$PP" 2>/dev/null; wait "$PP" 2>/dev/null
  PP=""

  if [ -z "$da" ] || [ -z "$d_p50" ] || [ -z "$d_p99" ] ||
     [ -z "$d_p999" ] || [ -z "$d_max" ] || [ -z "$pa" ] ||
     [ -z "$p_p50" ] || [ -z "$p_p99" ] || [ -z "$p_p999" ] ||
     [ -z "$p_max" ] || [ -z "$bl" ] ||
     [ -z "$d_completed" ] || [ -z "$d_errors" ] ||
     [ -z "$p_completed" ] || [ -z "$p_errors" ] ||
     [ "$d_completed" = 0 ] || [ "$p_completed" = 0 ]; then
    echo "饱和度测量缺少有效样本或统计字段，拒绝生成结果" >&2
    exit 1
  fi
  printf "%-9s | %-22s | %-38s | %s\n" "$RPS" \
    "$da ($d_p99)" \
    "$pa / $p_p50 / $p_p99 / $p_p999 / $p_max" \
    "$bl" | tee -a "$OUT"
  printf "          completed/errors: direct=%s/%s proxy=%s/%s\n" \
    "$d_completed" "$d_errors" "$p_completed" "$p_errors" | tee -a "$OUT"
  awk -v ticks="$((proxy_cpu_end-proxy_cpu_start))" -v hz="$ticks_per_s" \
      -v rss="$proxy_rss" 'BEGIN {printf "          gateway resources: cpu=%.2fs peak_rss=%sKiB\n", ticks/hz, rss}' \
      | tee -a "$OUT"
done
echo "Logs in $RESDIR/ (stamp $STAMP)" | tee -a "$OUT"
