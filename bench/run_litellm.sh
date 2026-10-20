#!/usr/bin/env bash

# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

# Head-to-head. Same open-loop loadgen, same 200 ms mock backend, same RPS
# levels: once through LiteLLM, once direct to the mock. Added gateway latency
# = (through-LiteLLM e2e) - (direct e2e). Pairs with run_bench.sh's llmbridge
# numbers to build the side-by-side artifact.
set -uo pipefail
cd "$(dirname "$0")/.."

LAT_MS="${1:-200}"
DUR="${2:-15}"
WARMUP="${3:-5}"
shift $(( $# < 3 ? $# : 3 )) || true
RPS_LIST=(${@:-100 500 1000 5000})

MOCK_PORT=9001
LL_PORT=8089
VENV=bench/.litellm-venv
RESDIR=bench/results
mkdir -p "$RESDIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$RESDIR/litellm-$STAMP.txt"

MOCK_PID=""
LL_PID=""
cleanup() {
  for pid in "$LL_PID" "$MOCK_PID"; do
    if [ -n "$pid" ]; then
      kill -TERM "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT

# Locate CMake-built loadgen (CLion uses cmake-build-*/bin). Override: BIN=path
BIN="${BIN:-}"
if [ -z "$BIN" ]; then
  for d in cmake-build-release/bin cmake-build-debug/bin build-linux/bin build/bin; do
    if [ -x "$d/loadgen" ]; then BIN="$d"; break; fi
  done
fi
[ -z "$BIN" ] && { echo "No built loadgen; build the CMake project first or set BIN=path" >&2; exit 1; }
[ -x "$VENV/bin/litellm" ] || { echo "LiteLLM 环境不存在：$VENV/bin/litellm" >&2; exit 1; }

python3 bench/mock_provider.py --port "$MOCK_PORT" --latency-ms "$LAT_MS" >"$RESDIR/mock-ll-$STAMP.log" 2>&1 &
MOCK_PID=$!
sleep 0.5
if ! kill -0 "$MOCK_PID" 2>/dev/null; then
  echo "Mock 启动失败或端口 $MOCK_PORT 已占用" >&2
  exit 1
fi

echo "Starting LiteLLM proxy ..."
"$VENV/bin/litellm" --config bench/litellm_config.yaml --port "$LL_PORT" >"$RESDIR/litellm-server-$STAMP.log" 2>&1 &
LL_PID=$!
ll_up=0
for i in $(seq 1 60); do
  if ! kill -0 "$LL_PID" 2>/dev/null; then break; fi
  curl -fsS -o /dev/null "http://127.0.0.1:$LL_PORT/health/liveliness" 2>/dev/null && { echo "LiteLLM up (${i}s)"; ll_up=1; break; }
  sleep 1
done
[ "$ll_up" = 1 ] || { echo "LiteLLM 未就绪，查看 $RESDIR/litellm-server-$STAMP.log" >&2; exit 1; }

extract() { sed -n 's/.*p50_us=\([0-9]*\) p99_us=\([0-9]*\) p999_us=\([0-9]*\) max_us=\([0-9]*\).*/\1 \2 \3 \4/p'; }
extract_counts() { sed -n 's/.*completed=\([0-9]*\) errors=\([0-9]*\) .*/\1 \2/p'; }
ms() { awk -v u="$1" 'BEGIN{printf "%.2f", u/1000.0}'; }

{
  echo "LiteLLM head-to-head, $STAMP  (mock ${LAT_MS}ms, ${DUR}s/level, ${WARMUP}s warmup)"
  printf "%-7s | %-30s | %-30s | %s\n" "RPS" "DIRECT-to-mock e2e (ms)" "THROUGH-LiteLLM e2e (ms)" "LiteLLM ADDED (ms)"
  printf "%-7s | %-30s | %-30s | %s\n" "" "p50 / p99 / p99.9 / max" "p50 / p99 / p99.9 / max" "Δp50 / Δp99"
  echo "--------+--------------------------------+--------------------------------+--------------------"
} | tee "$OUT"

for RPS in "${RPS_LIST[@]}"; do
  echo ">>> RPS=$RPS" >&2
  D=$($BIN/loadgen --target 127.0.0.1:$MOCK_PORT --rps "$RPS" --duration "$DUR" --warmup "$WARMUP" 2>>"$RESDIR/ll-direct-$RPS-$STAMP.log")
  read -r d50 d99 d999 dmax <<< "$(echo "$D" | extract)"
  read -r d_completed d_errors <<< "$(echo "$D" | extract_counts)"
  L=$($BIN/loadgen --target 127.0.0.1:$LL_PORT --rps "$RPS" --duration "$DUR" --warmup "$WARMUP" 2>>"$RESDIR/ll-proxy-$RPS-$STAMP.log")
  read -r l50 l99 l999 lmax <<< "$(echo "$L" | extract)"
  read -r l_completed l_errors <<< "$(echo "$L" | extract_counts)"
  if [ -z "$d50" ] || [ -z "$l50" ] || [ -z "$d_completed" ] || [ -z "$l_completed" ] ||
     [ "$d_completed" = 0 ] || [ "$l_completed" = 0 ]; then
    echo "loadgen 缺少有效样本，拒绝生成延迟对比结果" >&2
    exit 1
  fi
  a50=$(awk -v a="$l50" -v b="$d50" 'BEGIN{printf "%.2f", (a-b)/1000.0}')
  a99=$(awk -v a="$l99" -v b="$d99" 'BEGIN{printf "%.2f", (a-b)/1000.0}')
  printf "%-7s | %-30s | %-30s | %s\n" "$RPS" \
    "$(ms "$d50") / $(ms "$d99") / $(ms "$d999") / $(ms "$dmax")" \
    "$(ms "$l50") / $(ms "$l99") / $(ms "$l999") / $(ms "$lmax")" \
    "Δp50=$a50 Δp99=$a99" | tee -a "$OUT"
  printf "        completed/errors: direct=%s/%s LiteLLM=%s/%s\n" \
    "$d_completed" "$d_errors" "$l_completed" "$l_errors" | tee -a "$OUT"
done
echo "Logs in $RESDIR/ (stamp $STAMP)" | tee -a "$OUT"
