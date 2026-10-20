#!/usr/bin/env bash

# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

# Large request bodies through the gateway, sent at a throttled rate (bigbody): the
# venue's first byte, its last byte and the reply, per shape and size, for a
# byte-forward with --stream-openai-body on and off, and for a translated request.
#
#   BIN=build/bin ./bench/run_large_body.sh [mbit] [reps] [sizes...]
#   (also: LARGE=1 ./bench/run_bench.sh [mbit] [reps] [sizes...])
set -euo pipefail
cd "$(dirname "$0")/.."

MBIT="${1:-100}"
REPS="${2:-9}"
shift $(( $# < 2 ? $# : 2 )) || true
SIZES=("${@:-1 4 10}")
if [ "${#SIZES[@]}" -eq 1 ]; then read -r -a SIZES <<< "${SIZES[0]}"; fi
BIN="${BIN:-build/bin}"
GW=18088
VENUE=19001

run() { # <label> <dialect> <reply> <shape> <mb> [gateway flags...]
  local label=$1 dialect=$2 reply=$3 shape=$4 mb=$5; shift 5
  "$BIN/llmbridge" --legacy-mode --listen $GW --upstream 127.0.0.1:$VENUE --upstream-dialect "$dialect" \
      --workers 1 --duration 600 "$@" > /dev/null 2>&1 &
  local pid=$!
  sleep 0.5
  printf '%-22s ' "$label"
  "$BIN/bigbody" --gateway $GW --venue $VENUE --shape "$shape" --mb "$mb" --mbit "$MBIT" \
      --reps "$REPS" --reply "$reply" || echo "(failed)"
  kill -TERM $pid 2>/dev/null || true
  wait $pid 2>/dev/null || true
}

for MB in "${SIZES[@]}"; do
  for SHAPE in transcript schemas images; do
    run "forward, streamed" openai openai "$SHAPE" "$MB"
    run "forward, held" openai openai "$SHAPE" "$MB" --stream-openai-body off
    [ "$SHAPE" = images ] && continue # the translator refuses vision
    run "translated" anthropic anthropic "$SHAPE" "$MB"
    run "translated, retain 16" anthropic anthropic "$SHAPE" "$MB" --retain-mb 16
  done
done
