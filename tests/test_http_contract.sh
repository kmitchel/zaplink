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
while [[ $# -gt 0 ]]; do
    case "$1" in
        -c) config=$2; shift 2;;
        *) station=$1; shift;;
    esac
done
[[ "$station" == zaplink-selected ]]
[[ $(grep -c '^FREQUENCY' "$config") == 1 ]]
[[ $(grep -c '^\[' "$config") == 1 ]]
frequency=$(sed -n 's/^FREQUENCY = //p' "$config")
service=$(sed -n 's/^SERVICE_ID = //p' "$config")
printf '%s %s\n' "$frequency" "$service" >> "$ZAP_TEST_RECORD"
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
ZAP_TEST_RECORD="$temporary_dir/tuning" PATH="$temporary_dir/bin:$PATH" ZAPLINK_DB_PATH="$temporary_dir/epg.db" \
    ZAPLINK_CHANNELS_PATH="$repo_dir/tests/fixtures/channels.conf" \
    "$repo_dir/build/zaplink_no_tuner_test" -p "$port" \
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

# Select the other station with the same VCN and inspect the config seen by zap.
curl -sS --max-time 1 -o "$temporary_dir/duplicate.body" \
    "$base_url/stream/15.1-587000000-7.ts?latency=low" 2>/dev/null || true
[[ -s "$temporary_dir/duplicate.body" ]]
grep -q '^581000000 3$' "$temporary_dir/tuning"
grep -q '^587000000 7$' "$temporary_dir/tuning"
# No Host header: the generated URL must use the bound port.
curl -fsS --http1.0 -H 'Host:' "$base_url/playlist.m3u" > "$temporary_dir/no-host.m3u"
grep -q "http://localhost:$port/stream/" "$temporary_dir/no-host.m3u"
grep -q 'tvg-name="MARKET-B & '\''TV'\''"' "$temporary_dir/no-host.m3u"

python3 - "$port" <<'PYHTTP'
import socket
import sys
port = int(sys.argv[1])
def playlist(headers, path=b"/playlist.m3u", method=b"GET"):
    with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
        sock.sendall(method + b" " + path + b" HTTP/1.1\r\n" + headers + b"\r\n")
        chunks = []
        while data := sock.recv(8192):
            chunks.append(data)
    return b"".join(chunks)
for host in (b'bad"host', b'bad/path', b'user@host', b'bad#fragment', b'bad?query',
             b'bad host', b'bad,host', b'bad\thost', b'bad\nhost', b'bad\x01host',
             b'[fe80::1%2]', b'[fe80::1%25]', b'[fe80::1%25bad/path]', b'[fe80::1%25bad%0a]', b'host:0', b'host:65536', b'host:abc', b'[not-ipv6]', b'host%0a', b'', b'a'*256):
    response = playlist(b"Host: " + host + b"\r\n")
    assert response.startswith(b"HTTP/1.1 400"), (host, response)
response = playlist(b"Host: good.example\r\nhost: other.example\r\n")
assert response.startswith(b"HTTP/1.1 400"), response
for header, authority in ((b"hOsT:tv.example:1234\r\n", b"tv.example:1234"),
                          (b"Host: [::1]:4567\r\n", b"[::1]:4567"),
                          (b"Host: zaplink_service:1234\r\n", b"zaplink_service:1234"),
                          (b"Host: [fe80::1%25eth0]:4567\r\n", b"[fe80::1%25eth0]:4567"),
                          (b"Host: [fe80::1%eth0]:4567\r\n", b"[fe80::1%25eth0]:4567"),
                          (b"Host: [fe80::1%2525]:4567\r\n", b"[fe80::1%2525]:4567"),
                          (b"Host: [fe80::1%25250]:4567\r\n", b"[fe80::1%25250]:4567"),
                          (b"Host: [fe80::1%250]:4567\r\n", b"[fe80::1%250]:4567"),
                          (b"X-Host: ignored/host\r\n", f"localhost:{port}".encode())):
    response = playlist(header)
    assert response.startswith(b"HTTP/1.1 200"), response
    assert b"http://" + authority + b"/stream/" in response, response
    assert b"INVALID-VCN" not in response and b'injected=' not in response, response
    assert playlist(header, b"/xmltv.xml").startswith(b"HTTP/1.1 200")
    assert playlist(header, b"/stream/15.1-581000000-3.ts", b"HEAD").startswith(b"HTTP/1.1 200")
# The encoded delimiter takes precedence: %250 means scope 0, not raw 250.
from urllib.parse import unquote, urlsplit
for wire, scope in ((b"%2525", "25"), (b"%25250", "250"), (b"%250", "0")):
    response = playlist(b"Host: [fe80::1" + wire + b"]:4567\r\n")
    url = next(line for line in response.splitlines() if line.startswith(b"http://"))
    assert unquote(urlsplit(url.decode()).hostname).split("%", 1)[1] == scope, url
print("Playlist Host and malformed channel metadata tests: OK")
PYHTTP

if ZAPLINK_DB_PATH="$temporary_dir/second.db" \
    ZAPLINK_CHANNELS_PATH="$repo_dir/tests/fixtures/channels.conf" \
    "$repo_dir/build/zaplink_no_tuner_test" -p "$port" \
    >"$temporary_dir/bind-failure.log" 2>&1; then
    echo 'second server unexpectedly succeeded on an occupied port' >&2
    exit 1
fi
grep -q 'Failed to bind' "$temporary_dir/bind-failure.log"

mkdir -p "$temporary_dir/empty-path"
failure_port=$((port + 1))
PATH="$temporary_dir/empty-path" \
    ZAPLINK_DB_PATH="$temporary_dir/failure.db" \
    ZAPLINK_CHANNELS_PATH="$repo_dir/tests/fixtures/channels.conf" \
    "$repo_dir/build/zaplink_no_tuner_test" -p "$failure_port" \
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
