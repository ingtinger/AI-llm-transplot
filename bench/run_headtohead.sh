#!/usr/bin/env bash

# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

# Equal-work head-to-head: llmbridge vs LiteLLM, both doing the full OpenAI<->Anthropic
# round-trip (the work LiteLLM's transform_request/transform_response is built
# around), against the same mock backend, same open-loop loadgen.
#
#   - mock_provider.py --format anthropic        (returns an Anthropic Messages body)
#   - llmbridge:  --upstream-dialect anthropic             (OpenAI in -> Anthropic upstream -> OpenAI out)
#   - LiteLLM: litellm_config_anthropic.yaml     (anthropic/* model -> same mock)
#
# Per RPS we measure with the same loadgen: direct-to-mock (baseline), through
# llmbridge, through LiteLLM. llmbridge added latency = its own self-reported added-total
# p99 (request-path + response-path, upstream wait excluded). LiteLLM added latency
# = through-LiteLLM e2e - direct-to-mock e2e (it can't self-report). Emits a table
# plus bench/results/phase-a-comparison.csv for make_chart.py.
#
#   ./bench/run_headtohead.sh [latency_ms] [duration] [warmup] [rps_list...]
set -uo pipefail
cd "$(dirname "$0")/.."

# LiteLLM must use its bundled model-cost map; otherwise it hangs on a startup
# network fetch in a sandboxed/offline box.
export PYTHONUNBUFFERED=1 LITELLM_LOCAL_MODEL_COST_MAP=True \
       LITELLM_DONT_SHOW_FEEDBACK_BOX=True DISABLE_LLM_UPDATE_CHECK=1

LAT_MS="${1:-200}"
DUR="${2:-10}"
WARMUP="${3:-3}"
shift $(( $# < 3 ? $# : 3 )) || true
RPS_LIST=("${@:-100 500 1000 5000}")
BODY="${BODY:-64}" # loadgen request body size (bytes)
if [ "${#RPS_LIST[@]}" -eq 1 ]; then read -r -a RPS_LIST <<< "${RPS_LIST[0]}"; fi

MOCK_PORT=9001
LLMBRIDGE_PORT=8088
LL_PORT=8089
# Overridable so rerun_litellm.sh can point at a venv holding a different LiteLLM
# release without disturbing the pinned 1.95.0 one the published figures came from.
VENV="${LITELLM_VENV:-bench/.litellm-venv}"
RESDIR=bench/results
mkdir -p "$RESDIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
SUMMARY="$RESDIR/headtohead-$STAMP.txt"
# Overridable: rerun_litellm.sh points each arm at its own file so a re-measurement
# cannot overwrite the published run that feeds make_chart.py.
CSV="${H2H_CSV:-$RESDIR/phase-a-comparison.csv}"

BIN="${BIN:-}"
if [ -z "$BIN" ]; then
  for d in cmake-build-release/bin cmake-build-debug/bin build-linux/bin build/bin; do
    if [ -x "$d/llmbridge" ] && [ -x "$d/loadgen" ]; then BIN="$d"; break; fi
  done
fi
[ -z "$BIN" ] && { echo "No built binaries; build first or set BIN=path" >&2; exit 1; }
[ -x "$VENV/bin/litellm" ] || { echo "LiteLLM 环境不存在：$VENV/bin/litellm" >&2; exit 1; }
echo "Using binaries in $BIN"

MOCK_PID=""
LL_PID=""
KP=""
CSV_TMP=""
cleanup() {
  for pid in "$KP" "$LL_PID" "$MOCK_PID"; do
    if [ -n "$pid" ]; then
      kill -TERM "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
  if [ -n "$CSV_TMP" ]; then rm -f -- "$CSV_TMP"; fi
}
trap cleanup EXIT

echo "Starting mock (anthropic, ${LAT_MS}ms) ..."
python3 bench/mock_provider.py --port "$MOCK_PORT" --latency-ms "$LAT_MS" --format anthropic \
  >"$RESDIR/mock-h2h-$STAMP.log" 2>&1 &
MOCK_PID=$!
sleep 0.5
if ! kill -0 "$MOCK_PID" 2>/dev/null; then
  echo "Mock 启动失败或端口 $MOCK_PORT 已占用" >&2
  exit 1
fi

echo "Starting LiteLLM (anthropic config); may take ~30-45s to import ..."
"$VENV/bin/litellm" --config bench/litellm_config_anthropic.yaml --port "$LL_PORT" \
  >"$RESDIR/litellm-server-$STAMP.log" 2>&1 &
LL_PID=$!
ll_up=0
for i in $(seq 1 90); do
  if ! kill -0 "$LL_PID" 2>/dev/null; then break; fi
  curl -fsS -o /dev/null "http://127.0.0.1:$LL_PORT/health/liveliness" 2>/dev/null && { echo "LiteLLM up (${i}s)"; ll_up=1; break; }
  sleep 1
done
[ "$ll_up" = 1 ] || { echo "LiteLLM 未就绪，不能生成对比结果；查看 $RESDIR/litellm-server-$STAMP.log" >&2; exit 1; }

extract() { sed -n 's/.*p50_us=\([0-9]*\) p99_us=\([0-9]*\) p999_us=\([0-9]*\) max_us=\([0-9]*\).*/\1 \2 \3 \4/p'; }
extract_counts() { sed -n 's/.*completed=\([0-9]*\) errors=\([0-9]*\) .*/\1 \2/p'; }
achieved_of() { sed -n 's/.*achieved=\([0-9]*\) .*/\1/p'; }
ms() { awk -v u="$1" 'BEGIN{printf "%.3f", u/1000.0}'; }

{
  echo "llmbridge vs LiteLLM, equal-work (OpenAI<->Anthropic), $STAMP"
  echo "mock(anthropic) latency=${LAT_MS}ms  dur=${DUR}s  warmup=${WARMUP}s  host=$(uname -mn)"
  echo
  printf "%-6s | %-22s | %-30s | %s\n" "RPS" "LLMBRIDGE added (ms)" "LITELLM added (ms)" "LiteLLM achieved"
  printf "%-6s | %-22s | %-30s | %s\n" "" "self p50 / p99" "Δp50 / Δp99 (client)" "req/s (target)"
  echo "-------+------------------------+--------------------------------+------------------"
} | tee "$SUMMARY"

CSV_TMP=$(mktemp "${CSV}.tmp.XXXXXX") || exit 1
echo "# llmbridge vs LiteLLM, equal-work OpenAI<->Anthropic, $STAMP, host=$(uname -mn), mock ${LAT_MS}ms" > "$CSV_TMP"
echo "rps,llmbridge_p50_ms,llmbridge_p99_ms,llmbridge_p999_ms,llmbridge_max_ms,litellm_p50_ms,litellm_achieved,litellm_p99_ms,litellm_max_ms,litellm_sat" >> "$CSV_TMP"

for RPS in "${RPS_LIST[@]}"; do
  echo ">>> RPS=$RPS" >&2

  # (1) direct-to-mock baseline (shared by both deltas)
  D=$($BIN/loadgen --target 127.0.0.1:$MOCK_PORT --rps "$RPS" --duration "$DUR" --warmup "$WARMUP" --body-bytes "$BODY" 2>>"$RESDIR/h2h-direct-$RPS-$STAMP.log")
  read -r d50 d99 d999 dmax <<< "$(echo "$D" | extract)"
  read -r d_completed d_errors <<< "$(echo "$D" | extract_counts)"
  : "${d50:=0}" "${d99:=0}" "${d999:=0}" "${dmax:=0}"

  # (2) through llmbridge (fresh per RPS; translate anthropic = equal work)
  $BIN/llmbridge --legacy-mode --listen $LLMBRIDGE_PORT --upstream 127.0.0.1:$MOCK_PORT --upstream-dialect anthropic --io uring --workers 1 \
      --duration $((DUR + WARMUP + 3)) --warmup "$WARMUP" >"$RESDIR/h2h-llmbridge-$RPS-$STAMP.log" 2>&1 &
  KP=$!
  sleep 1
  K=$($BIN/loadgen --target 127.0.0.1:$LLMBRIDGE_PORT --rps "$RPS" --duration "$DUR" --warmup "$WARMUP" --body-bytes "$BODY" 2>>"$RESDIR/h2h-llmbridge-loadgen-$RPS-$STAMP.log")
  read -r k_completed k_errors <<< "$(echo "$K" | extract_counts)"
  kill -TERM "$KP" 2>/dev/null; wait "$KP" 2>/dev/null
  KP=""
  k_self_p50=$(sed -n 's/.*added-total.*p50=\([0-9.]*\) [µu]s.*/\1/p' "$RESDIR/h2h-llmbridge-$RPS-$STAMP.log" | head -1)
  k_self_p99=$(sed -n 's/.*added-total.*p99=\([0-9.]*\) [µu]s.*/\1/p' "$RESDIR/h2h-llmbridge-$RPS-$STAMP.log" | head -1)
  k_self_p999=$(sed -n 's/.*added-total.*p99.9=\([0-9.]*\) [µu]s.*/\1/p' "$RESDIR/h2h-llmbridge-$RPS-$STAMP.log" | head -1)
  k_self_max=$(sed -n 's/.*added-total.*max=\([0-9.]*\) [µu]s.*/\1/p' "$RESDIR/h2h-llmbridge-$RPS-$STAMP.log" | head -1)
  : "${k_self_p50:=0}" "${k_self_p99:=0}" "${k_self_p999:=0}" "${k_self_max:=0}"

  # (3) through LiteLLM
  L=$($BIN/loadgen --target 127.0.0.1:$LL_PORT --rps "$RPS" --duration "$DUR" --warmup "$WARMUP" --body-bytes "$BODY" 2>>"$RESDIR/h2h-litellm-$RPS-$STAMP.log")
  read -r l50 l99 l999 lmax <<< "$(echo "$L" | extract)"
  read -r l_completed l_errors <<< "$(echo "$L" | extract_counts)"
  if [ -z "$d50" ] || [ -z "$l50" ] || [ -z "$d_completed" ] ||
     [ -z "$k_completed" ] || [ -z "$l_completed" ] ||
     [ "$d_completed" = 0 ] || [ "$k_completed" = 0 ] || [ "$l_completed" = 0 ]; then
    echo "loadgen 缺少有效样本，拒绝生成延迟对比结果" >&2
    exit 1
  fi
  la=$(echo "$L" | achieved_of)
  : "${la:=0}"

  # LiteLLM added latency = client delta (ms)
  la50=$(awk -v a="$l50" -v b="$d50" 'BEGIN{printf "%.3f", (a-b)/1000.0}')
  la99=$(awk -v a="$l99" -v b="$d99" 'BEGIN{printf "%.3f", (a-b)/1000.0}')
  la999=$(awk -v a="$l999" -v b="$d999" 'BEGIN{printf "%.3f", (a-b)/1000.0}')
  lamax=$(awk -v a="$lmax" -v b="$dmax" 'BEGIN{printf "%.3f", (a-b)/1000.0}')
  # saturated if achieved < 90% of target
  lsat=$(awk -v a="$la" -v t="$RPS" 'BEGIN{print (a < 0.9*t) ? "yes" : "no"}')
  # llmbridge self-measured added, in ms
  k50=$(awk -v u="$k_self_p50" 'BEGIN{printf "%.3f", u/1000.0}')
  k99=$(awk -v u="$k_self_p99" 'BEGIN{printf "%.3f", u/1000.0}')
  k999=$(awk -v u="$k_self_p999" 'BEGIN{printf "%.3f", u/1000.0}')
  kmax=$(awk -v u="$k_self_max" 'BEGIN{printf "%.3f", u/1000.0}')

  printf "%-6s | %-22s | %-30s | %s\n" "$RPS" \
    "$k50 / $k99" \
    "Δp50=$la50 Δp99=$la99" \
    "$la / $RPS ($lsat sat)" | tee -a "$SUMMARY"
  printf "       completed/errors: direct=%s/%s llmbridge=%s/%s LiteLLM=%s/%s\n" \
    "$d_completed" "$d_errors" "$k_completed" "$k_errors" "$l_completed" "$l_errors" | tee -a "$SUMMARY"

  echo "$RPS,$k50,$k99,$k999,$kmax,$la50,$la,$la99,$lamax,$lsat" >> "$CSV_TMP"
done

mv -- "$CSV_TMP" "$CSV" || exit 1
CSV_TMP=""
echo | tee -a "$SUMMARY"
echo "CSV: $CSV   (feeds make_chart.py)" | tee -a "$SUMMARY"
echo "Raw logs in $RESDIR/ (stamp $STAMP)" | tee -a "$SUMMARY"
echo "Done."
