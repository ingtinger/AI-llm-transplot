#!/usr/bin/env bash

# AI-llm-transplot 扩展：在真实 Docker Compose 中验证 TLS、双 Mock 与故障切换。
# Licensed under the Apache License, Version 2.0.

set -euo pipefail
cd "$(dirname "$0")"

command -v docker >/dev/null || { echo "需要 Docker Engine" >&2; exit 1; }
docker compose version >/dev/null
command -v curl >/dev/null || { echo "需要 curl" >&2; exit 1; }
command -v openssl >/dev/null || { echo "需要 openssl" >&2; exit 1; }

compose=(docker compose --env-file .env)
cleanup() {
  "${compose[@]}" down --volumes --remove-orphans >/dev/null 2>&1 || true
  rm -rf certs .env
}
trap cleanup EXIT

cleanup
mkdir -p certs
umask 077
cat >.env <<EOF
GATEWAY_UID=$(id -u)
GATEWAY_GID=$(id -g)
PROVIDER_PRIMARY_KEY=demo-primary-key-only
PROVIDER_BACKUP_KEY=demo-backup-key-only
GATEWAY_ALICE_KEY=demo-gateway-key-change-me
EOF
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
  -subj '/CN=localhost' \
  -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' \
  -keyout certs/key.pem -out certs/cert.pem >/dev/null 2>&1

"${compose[@]}" up --build -d

request() {
  curl --silent --show-error --fail --max-time 5 \
    --cacert certs/cert.pem https://127.0.0.1:8088/v1/chat/completions \
    -H 'Content-Type: application/json' \
    -H 'Authorization: Bearer demo-gateway-key-change-me' \
    -d '{"model":"chat-default","messages":[{"role":"user","content":"ping"}]}'
}

ready=0
for _ in $(seq 1 60); do
  if response=$(request 2>/dev/null) && grep -q '"content": "pong"' <<<"$response"; then
    ready=1
    break
  fi
  sleep 1
done
if [ "$ready" != 1 ]; then
  "${compose[@]}" ps >&2
  "${compose[@]}" logs --no-color >&2
  echo "网关未在期限内通过 TLS 返回 Mock 响应" >&2
  exit 1
fi

metrics() {
  "${compose[@]}" exec -T gateway timeout 5 bash -c \
    'exec 3<>/dev/tcp/127.0.0.1/9091; printf "GET /metrics HTTP/1.0\r\n\r\n" >&3; cat <&3'
}

primary=$(metrics | awk '$1 == "ai_gateway_upstream_route_selections_total{upstream=\"0\"}" {print $2}')
if [ -z "$primary" ] || [ "$primary" -lt 1 ]; then
  echo "首次请求未选择主上游" >&2
  exit 1
fi

"${compose[@]}" stop mock-primary
failed_over=0
for _ in $(seq 1 20); do
  request >/dev/null 2>&1 || true
  backup=$(metrics | awk '$1 == "ai_gateway_upstream_route_selections_total{upstream=\"1\"}" {print $2}')
  if [ -n "$backup" ] && [ "$backup" -ge 1 ]; then
    failed_over=1
    break
  fi
  sleep 1
done
if [ "$failed_over" != 1 ]; then
  "${compose[@]}" logs --no-color >&2
  echo "停止主 Mock 后没有观察到备用上游选路" >&2
  exit 1
fi

"${compose[@]}" start mock-primary
echo "Docker Compose TLS 与双上游故障切换验证通过"
