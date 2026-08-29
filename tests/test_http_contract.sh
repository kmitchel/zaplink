#!/usr/bin/env bash
set -euo pipefail

base_url=${1:-http://127.0.0.1:18392}
channel=${2:-15.1}
server_pid=${3:-}

before_children=''
if [[ -n "$server_pid" ]]; then
    before_children=$(pgrep -P "$server_pid" | sort || true)
fi

playlist=$(curl -fsS "$base_url/playlist.m3u")
grep -q "/stream/.*\.ts" <<<"$playlist"

typed_headers=$(curl -fsSI "$base_url/stream/$channel.ts?latency=low")
grep -qi '^Content-Type: video/mp2t' <<<"$typed_headers"
grep -qi '^X-Zaplink-Latency: low' <<<"$typed_headers"

legacy_headers=$(curl -fsSI "$base_url/stream/$channel")
grep -qi '^Content-Type: video/mp2t' <<<"$legacy_headers"

invalid_status=$(curl -sSI -o /dev/null -w '%{http_code}' \
    "$base_url/stream/$channel.ts?latency=fastest")
[[ "$invalid_status" == "400" ]]

if [[ -n "$server_pid" ]]; then
    after_children=$(pgrep -P "$server_pid" | sort || true)
    [[ "$before_children" == "$after_children" ]]
fi

echo "HTTP stream contract tests: OK"
