#!/usr/bin/env bash

# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

# Phase A benchmark orchestrator.
#
# For each target RPS we measure latency twice with the same open-loop loadgen:
#   (1) through the llmbridge proxy        -> backend latency + proxy overhead
#   (2) direct to the mock provider     -> backend latency baseline
# The proxy's added latency is the delta of those two client-observed numbers,
# cross-checked against the proxy's own self-reported overhead (request-path +
# response-path, with the upstream wait excluded). Pass = p99 added < 1 ms at
# 1000 RPS.
#
# Everything runs on one box (mock + proxy + loadgen compete for the same 10
# cores), so treat absolute tails as a dev-box upper bound; a clean number
# wants separate hosts (Phase D). The delta methodology cancels most co-location
# noise because both legs see the same contention.
#
#   ./bench/run_bench.sh [latency_ms] [duration] [warmup] [rps_list...]
set -euo pipefail
cd "$(dirname "$0")/.."

# LARGE=1: large bodies at a throttled rate instead (bench/run_large_body.sh).
if [ "${LARGE:-0}" = 1 ]; then exec bench/run_large_body.sh "$@"; fi

LAT_MS="${1:-200}"
DUR="${2:-20}"
WARMUP="${3:-5}"
shift $(( $# < 3 ? $# : 3 )) || true
RPS_LIST=("${@:-100 500 1000 5000}")
# allow a single quoted "100 500 ..." arg or separate args
if [ "${#RPS_LIST[@]}" -eq 1 ]; then read -r -a RPS_LIST <<< "${RPS_LIST[0]}"; fi

MOCK_PORT=9001
PROXY_PORT=8088
RESDIR="bench/results"
mkdir -p "$RESDIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
SUMMARY="$RESDIR/summary-$STAMP.txt"

# Locate CMake-built binaries (CLion uses cmake-build-*/bin). Override: BIN=path
BIN="${BIN:-}"
if [ -z "$BIN" ]; then
  for d in cmake-build-release/bin cmake-build-debug/bin build-linux/bin build/bin; do
    if [ -x "$d/llmbridge" ]; then BIN="$d"; break; fi
  done
fi
[ -z "$BIN" ] && { echo "No built binaries; build the CMake project first or set BIN=path" >&2; exit 1; }

MOCK_PID=""
PROXY_PID=""
cleanup() {
  for pid in "$PROXY_PID" "$MOCK_PID"; do
    if [ -n "$pid" ]; then
      kill -TERM "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT

echo "Using binaries in $BIN"

echo "Starting mock provider (latency=${LAT_MS}ms) ..."
python3 bench/mock_provider.py --port "$MOCK_PORT" --latency-ms "$LAT_MS" >"$RESDIR/mock-$STAMP.log" 2>&1 &
MOCK_PID=$!
sleep 1
if ! kill -0 "$MOCK_PID" 2>/dev/null; then
  echo "Mock 启动失败或端口 $MOCK_PORT 已占用" >&2
  exit 1
fi

{
  echo "llmbridge Phase A benchmark, $STAMP"
  echo "mock latency=${LAT_MS}ms  duration=${DUR}s  warmup=${WARMUP}s  host=$(uname -mn)"
  echo
  printf "%-7s | %-32s | %-32s | %s\n" "RPS" "DIRECT-to-mock (client e2e, ms)" "THROUGH-proxy (client e2e, ms)" "PROXY ADDED (ms)"
  printf "%-7s | %-32s | %-32s | %s\n" "" "p50 / p99 / p99.9 / max" "p50 / p99 / p99.9 / max" "delta-p50 / delta-p99 | self-p99"
  echo "--------+----------------------------------+----------------------------------+-----------------------------------"
} | tee "$SUMMARY"

# parse "p50_us=NNN p99_us=NNN p999_us=NNN max_us=NNN" -> echo "p50 p99 p999 max" in ms (3 decimals)
extract_us() { sed -n 's/.*p50_us=\([0-9]*\) p99_us=\([0-9]*\) p999_us=\([0-9]*\) max_us=\([0-9]*\).*/\1 \2 \3 \4/p'; }
extract_counts() { sed -n 's/.*completed=\([0-9]*\) errors=\([0-9]*\) .*/\1 \2/p'; }
ms() { awk -v u="$1" 'BEGIN{printf "%.3f", u/1000.0}'; }
cpu_ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }
peak_rss_kb() { awk '$1 == "VmHWM:" {print $2}' "/proc/$1/status" 2>/dev/null || echo 0; }
ticks_per_s=$(getconf CLK_TCK)

for RPS in "${RPS_LIST[@]}"; do
  echo ">>> RPS=$RPS" >&2

  # (2) direct-to-mock baseline
  DIRECT=$($BIN/loadgen --target 127.0.0.1:$MOCK_PORT --rps "$RPS" --duration "$DUR" --warmup "$WARMUP" 2>>"$RESDIR/loadgen-direct-$RPS-$STAMP.log")
  read -r d50 d99 d999 dmax <<< "$(echo "$DIRECT" | extract_us)"
  read -r d_completed d_errors <<< "$(echo "$DIRECT" | extract_counts)"

  # (1) through-proxy: fresh proxy per level so its self-stats are per-RPS
  $BIN/llmbridge --legacy-mode --listen $PROXY_PORT --upstream 127.0.0.1:$MOCK_PORT \
      --duration $((DUR + WARMUP + 3)) --warmup "$WARMUP" >"$RESDIR/proxy-$RPS-$STAMP.log" 2>&1 &
  PROXY_PID=$!
  sleep 1
  if ! kill -0 "$PROXY_PID" 2>/dev/null; then
    echo "网关启动失败或端口 $PROXY_PORT 已占用" >&2
    exit 1
  fi
  proxy_cpu_start=$(cpu_ticks "$PROXY_PID")
  PROXIED=$($BIN/loadgen --target 127.0.0.1:$PROXY_PORT --rps "$RPS" --duration "$DUR" --warmup "$WARMUP" 2>>"$RESDIR/loadgen-proxy-$RPS-$STAMP.log")
  proxy_cpu_end=$(cpu_ticks "$PROXY_PID")
  proxy_rss=$(peak_rss_kb "$PROXY_PID")
  read -r p50 p99 p999 pmax <<< "$(echo "$PROXIED" | extract_us)"
  read -r p_completed p_errors <<< "$(echo "$PROXIED" | extract_counts)"
  if [ -z "$d50" ] || [ -z "$d99" ] || [ -z "$d999" ] || [ -z "$dmax" ] ||
     [ -z "$p50" ] || [ -z "$p99" ] || [ -z "$p999" ] || [ -z "$pmax" ] ||
     [ -z "$d_completed" ] || [ -z "$d_errors" ] ||
     [ -z "$p_completed" ] || [ -z "$p_errors" ] ||
     [ "$d_completed" = 0 ] || [ "$p_completed" = 0 ]; then
    echo "loadgen 缺少有效样本或延迟字段，拒绝生成不完整基准结果" >&2
    exit 1
  fi
  kill -TERM "$PROXY_PID" 2>/dev/null || true; wait "$PROXY_PID" 2>/dev/null || true
  PROXY_PID=""

  # proxy self-reported added-total p99 (µs) from its dumped histogram. The
  # histogram prints "us"/"µs" for microseconds and "ns" for nanoseconds; handle
  # all three (us/µs values are already in µs; ns gets divided down).
  self_p99=$(sed -n 's/.*added-total.*p99=\([0-9.]*\) [µu]s.*/\1/p' "$RESDIR/proxy-$RPS-$STAMP.log" | head -1)
  [ -z "$self_p99" ] && self_p99=$(sed -n 's/.*added-total.*p99=\([0-9.]*\) ns.*/\1/p' "$RESDIR/proxy-$RPS-$STAMP.log" | head -1 | awk '{printf "%.3f", $1/1000}')
  [ -n "$self_p99" ] || { echo "网关自身延迟直方图缺失，拒绝生成不完整基准结果" >&2; exit 1; }

  # added latency delta (ms)
  add50=$(awk -v a="$p50" -v b="$d50" 'BEGIN{printf "%.3f", (a-b)/1000.0}')
  add99=$(awk -v a="$p99" -v b="$d99" 'BEGIN{printf "%.3f", (a-b)/1000.0}')

  printf "%-7s | %-32s | %-32s | %s\n" \
    "$RPS" \
    "$(ms "$d50") / $(ms "$d99") / $(ms "$d999") / $(ms "$dmax")" \
    "$(ms "$p50") / $(ms "$p99") / $(ms "$p999") / $(ms "$pmax")" \
    "Δp50=$add50 Δp99=$add99 | self-p99=$(awk -v u="$self_p99" 'BEGIN{printf "%.3f", u/1000.0}')ms" \
    | tee -a "$SUMMARY"
  printf "        completed/errors: direct=%s/%s proxy=%s/%s\n" \
    "$d_completed" "$d_errors" "$p_completed" "$p_errors" | tee -a "$SUMMARY"
  awk -v ticks="$((proxy_cpu_end-proxy_cpu_start))" -v hz="$ticks_per_s" \
      -v rss="$proxy_rss" 'BEGIN {printf "        gateway resources: cpu=%.2fs peak_rss=%sKiB\n", ticks/hz, rss}' \
      | tee -a "$SUMMARY"
done

echo | tee -a "$SUMMARY"
echo "Raw logs + per-RPS proxy histograms in $RESDIR/  (stamp $STAMP)" | tee -a "$SUMMARY"
echo "Done."
