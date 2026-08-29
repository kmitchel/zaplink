#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
temporary_dir=$(mktemp -d)
server_pid=''
failure_server_pid=''

cleanup() {
    if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
        kill -TERM "$server_pid"
        wait "$server_pid" || true
    fi
    if [[ -n "$failure_server_pid" ]] &&
       kill -0 "$failure_server_pid" 2>/dev/null; then
        kill -TERM "$failure_server_pid"
        wait "$failure_server_pid" || true
    fi
    rm -rf -- "$temporary_dir"
}
trap cleanup EXIT

mkdir -p "$temporary_dir/bin"
cat >"$temporary_dir/bin/dvbv5-zap" <<'SCRIPT'
#!/usr/bin/env bash
set -euo pipefail
packet=$(printf 'G%.0s' {1..188})
while :; do
    printf '%s' "$packet"
done
SCRIPT
cat >"$temporary_dir/bin/ffmpeg" <<'SCRIPT'
#!/usr/bin/env bash
for argument in "$@"; do
    if [[ "$argument" == 'matroska' ]]; then
        printf '\x1a\x45\xdf\xa3'
        break
    fi
done
exec cat
SCRIPT
chmod 755 "$temporary_dir/bin/dvbv5-zap" "$temporary_dir/bin/ffmpeg"

port=$((20000 + $$ % 20000))
base_url="http://127.0.0.1:$port"
PATH="$temporary_dir/bin:$PATH" ZAPLINK_DB_PATH="$temporary_dir/epg.db" \
    ZAPLINK_CHANNELS_PATH="$repo_dir/channels.conf" \
    "$repo_dir/build/zaplink" -p "$port" \
    >"$temporary_dir/server.log" 2>&1 &
server_pid=$!

for _ in $(seq 1 100); do
    if curl -fsS "$base_url/playlist.m3u" \
        >"$temporary_dir/playlist" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$server_pid" 2>/dev/null; then
        cat "$temporary_dir/server.log" >&2
        exit 1
    fi
    sleep 0.05
done
grep -q '/stream/.*\.ts' "$temporary_dir/playlist"
curl -fsS "$base_url/xmltv.xml" | grep -q '<tv'

channel=$(awk -F/ '/\/stream\// { sub(/\.ts.*/, "", $NF); print $NF; exit }' \
    "$temporary_dir/playlist")
[[ -n "$channel" ]]

before_children=$(pgrep -P "$server_pid" | sort || true)
typed_headers=$(curl -fsSI "$base_url/stream/$channel.ts?latency=low")
grep -qi '^Content-Type: video/mp2t' <<<"$typed_headers"
grep -qi '^X-Zaplink-Latency: low' <<<"$typed_headers"
legacy_headers=$(curl -fsSI "$base_url/stream/$channel")
grep -qi '^Content-Type: video/mp2t' <<<"$legacy_headers"
mkv_headers=$(curl -fsSI \
    "$base_url/stream/$channel.mkv?codec=h264&backend=software")
grep -qi '^Content-Type: video/x-matroska' <<<"$mkv_headers"
after_children=$(pgrep -P "$server_pid" | sort || true)
[[ "$before_children" == "$after_children" ]]

invalid_queries=(
    'unknown=value'
    'latency=low&latency=robust'
    'latency='
    'latency=%00low'
    'latency=%zz'
    'container=mkv'
)
for query in "${invalid_queries[@]}"; do
    status=$(curl -sSI -o /dev/null -w '%{http_code}' \
        "$base_url/stream/$channel.ts?$query")
    [[ "$status" == '400' ]]
done

for path in "$channel.ts?latency=low" "$channel?latency=low"; do
    curl -sS --max-time 1 -D "$temporary_dir/get.headers" \
        -o "$temporary_dir/get.body" "$base_url/stream/$path" 2>/dev/null || true
    grep -qi '^HTTP/1.1 200 OK' "$temporary_dir/get.headers"
    grep -qi '^Content-Type: video/mp2t' "$temporary_dir/get.headers"
    [[ -s "$temporary_dir/get.body" ]]
    [[ $(od -An -t x1 -N1 "$temporary_dir/get.body" | tr -d ' ') == '47' ]]
done
sleep 0.1
[[ $(grep -c 'Pipeline launched:' "$temporary_dir/server.log") -eq 1 ]]
grep -q 'Reusing producer' "$temporary_dir/server.log"

curl -sS --max-time 1 -D "$temporary_dir/mkv.headers" \
    -o "$temporary_dir/mkv.body" \
    "$base_url/stream/$channel.mkv?codec=h264&backend=software" \
    2>/dev/null || true
grep -qi '^Content-Type: video/x-matroska' "$temporary_dir/mkv.headers"
[[ $(od -An -t x1 -N4 "$temporary_dir/mkv.body" | tr -d ' \n') == \
   '1a45dfa3' ]]

if ZAPLINK_DB_PATH="$temporary_dir/second.db" \
    ZAPLINK_CHANNELS_PATH="$repo_dir/channels.conf" \
    "$repo_dir/build/zaplink" -p "$port" \
    >"$temporary_dir/bind-failure.log" 2>&1; then
    echo 'second server unexpectedly succeeded on an occupied port' >&2
    exit 1
fi
grep -q 'Failed to bind' "$temporary_dir/bind-failure.log"

mkdir -p "$temporary_dir/empty-path"
failure_port=$((port + 1))
PATH="$temporary_dir/empty-path" \
    ZAPLINK_DB_PATH="$temporary_dir/failure.db" \
    ZAPLINK_CHANNELS_PATH="$repo_dir/channels.conf" \
    "$repo_dir/build/zaplink" -p "$failure_port" \
    >"$temporary_dir/launch-failure.log" 2>&1 &
failure_server_pid=$!
for _ in $(seq 1 100); do
    if curl -fsS "http://127.0.0.1:$failure_port/playlist.m3u" \
        >/dev/null 2>&1; then
        break
    fi
    sleep 0.05
done
failure_status=$(curl -sS -o "$temporary_dir/launch-failure.body" \
    -w '%{http_code}' "http://127.0.0.1:$failure_port/stream/$channel.ts")
[[ "$failure_status" == '502' ]]
grep -q 'Unable to start dvbv5-zap' "$temporary_dir/launch-failure.body"

echo 'HTTP stream contract tests: OK'
