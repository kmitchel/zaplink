# ZapLink

ZapLink is a lightweight ATSC/DVB backend that scans channels, builds an EPG from PSIP tables, and serves live streams and guide data over HTTP for clients like Jellyfin.

## Security Features

ZapLink has been hardened for production environments:

-   **Shell Removal**: The transcoding pipeline uses `posix_spawnp()` with explicit file actions and argument arrays, avoiding both shell injection and unsafe post-`fork()` setup in the multithreaded server.
-   **Argument Safety**: FFmpeg argument construction has a fixed 128-entry limit and refuses to launch a partial command if that limit is exceeded.
-   **HTTP Robustness**: The request parser enforces 4KB headers, a 10-second header deadline, bounded pending connections, exact query parsing, and strict parameter ranges.
-   **Bounded Streaming**: Pipelines that produce no data or stall for 15 seconds are terminated and return an HTTP error when possible.

## Features
- **Automatic Channel Scan**: Parallel tuner scanning with RabbitEars zip-code hints and optional VHF skipping.
- **Reliable Device Startup**: Waits up to 30 seconds for USB DVB frontends before scanning or serving streams.
- **Weak-Signal Filtering**: Measures each scanned multiplex and comments out channels below 20 dB C/N unless explicitly retained.
- **Robust EPG**: Low-overhead background EPG collection with valid XMLTV output supporting Jellyfin Series Recording and bounded memory footprint.
- **Stable Channel Identity**: Precomputed collision-free identifiers keep playlist, XMLTV, and stream routing consistent.
- **Live Streaming**: Supports software and hardware transcoding (QSV, VAAPI, NVENC) via FFmpeg.
- **Fast Stream Handoffs**: Compatible MPEG-TS requests share one normalized producer and remain warm briefly across probe-to-playback reconnects.
- **Simple API**: HTTP endpoints for M3U playlists and XMLTV guide data.
- **Journal Logging**: Plain key/value logs use syslog priority prefixes understood by journald and contain no terminal color sequences.

## Requirements
- Linux with DVB/ATSC hardware (`/dev/dvb/adapter*`).
- `dvbv5-scan` and `dvbv5-zap` (from `v4l-utils` or `dvb-apps`).
- `ffmpeg` (for streaming/transcoding).
- SQLite3 (development headers).
- `curl` (for scan hints).
- `make` and `gcc`.

## Build & Install

### Standard Build
Build using system libraries:
```sh
make
sudo make install
```

### Local Build (Bundled Deps)
If system SQLite is missing/old, use the setup script and local build target:
```sh
./support/setup_env.sh
make
sudo make install
```

### Installation Details
`make install` will:
- Install binary to `/opt/zaplink/zaplink`.
- Install `huffman.bin` to `/opt/zaplink/`.
- Create a `zaplink` user.
- Install and enable the `zaplink.service`.
- Add the service account to available `video` and `render` groups.
- Create `/var/lib/zaplink` for mutable EPG state.
- Install a hardened unit that keeps `/opt/zaplink` read-only to the service and permits writes only below `/var/lib/zaplink`.

The binary, unit, and static assets are installed as `root:root`. Do not make
the executable or rollback copies writable by the `zaplink` service account.

**Note:** `channels.conf` is *not* overwritten or installed by default, to
protect an existing tuner configuration. Before the first packaged-service
start, copy or scan it into `/var/lib/zaplink/channels.conf`.

## Usage

### Managing the Service
Start, stop, or check the status of the ZapLink engine:
```sh
sudo systemctl start zaplink
sudo systemctl stop zaplink
sudo systemctl status zaplink
```
Logs can be viewed via journalctl:
```sh
sudo journalctl -u zaplink -f
```

Log records are single-line, color-free entries suitable for filtering and
forwarding. For example:

```text
timestamp=2026-08-09T20:17:54Z level=INFO component=TUNER message=2 usable DVB frontends ready
```

### Manual Usage
You can run the binary directly for testing:
```sh
./build/zaplink -v
```
Flags:
- `-p <port>`: HTTP port (default `18392`).
- `-v`: Verbose logging.
- `-e`: Explicitly enable periodic EPG tuner scans. Scanning is disabled by default.
- `-n`: Disable the EPG database and XMLTV endpoint entirely.
- `-t`: Run transcoding benchmark.
- `-s`: Force channel rescan.
- `-h`: Show command-line help.

### DVB Startup Behavior

USB DVB devices may finish initializing after systemd starts ZapLink. At startup,
ZapLink therefore checks for frontend nodes matching
`/dev/dvb/adapter*/frontend*` and waits up to 30 seconds for at least one that
the service account can read and write. Only adapters with usable frontend
nodes are added to the tuner pool.

The journal reports the result:

```text
TUNER 2 usable DVB frontends ready
TUNER Discovered 2 tuners
```

If the timeout expires, ZapLink continues starting its HTTP interface but has no
tuners available for streaming. If a tuner appears after the timeout, restart
the service to repeat discovery:

```sh
sudo systemctl restart zaplink
```

The `zaplink` account must have read/write access to the frontend, demux, and DVR
nodes, typically through membership in the `video` group.

### EPG Operation

The XMLTV database and periodic tuner scanner are separate concerns. ZapLink
opens the database and serves `/xmltv.xml` by default, but it does not reserve a
tuner for periodic EPG collection unless started with `-e`. This is appropriate
when guide data is populated externally or the host must reserve all tuners for
viewing and recording. Add `-e` to `ExecStart` only when ZapLink should collect
PSIP guide data itself. Use `-n` when neither the database nor XMLTV endpoint is
needed.

The packaged service stores `epg.db` and `channels.conf` below
`/var/lib/zaplink` through `ZAPLINK_DB_PATH` and `ZAPLINK_CHANNELS_PATH`. Manual
runs without those variables continue using the current directory for
compatibility.

### Channel Scanning and Signal Filtering

The interactive scanner can be run from the installed directory. Stop the
service first so the scanner has exclusive access to the tuners. It writes and
validates a candidate file first; the active configuration remains untouched
after any scan failure and is replaced atomically only after success:

```sh
sudo systemctl stop zaplink
cd /opt/zaplink
sudo -u zaplink env ZAPLINK_CHANNELS_PATH=/var/lib/zaplink/channels.conf ./zaplink -s
sudo systemctl start zaplink
```

The scanner asks whether to:

1. Run the automatic channel scan.
2. Use a ZIP code for RabbitEars frequency hints.
3. Skip VHF RF channels 2–13.
4. Include weak channels below 20 dB C/N.

The setup wizard requires an interactive terminal. If `channels.conf` is
missing during systemd startup, ZapLink exits with a clear error instead of
repeatedly prompting on closed standard input.

After discovery, ZapLink tunes one representative service from every unique RF
multiplex and averages the lock samples reported by `dvbv5-zap`. A multiplex is
classified as weak when it has no tuner lock or its measured C/N is below
20 dB. Because every virtual subchannel on a multiplex shares the same RF
signal, the decision applies to all services on that frequency.

The weak-channel prompt defaults to **No**. When weak channels are excluded,
their complete blocks remain in `channels.conf` as comments, including the
measurement that caused the exclusion:

```ini
# ZapLink disabled weak multiplex: -72.0 dBm, 17.8 dB C/N (minimum 20.0 dB)
# [WFWC-CD]
#     VCHANNEL = 45.1
#     SERVICE_ID = 1001
#     FREQUENCY = 485000000
```

Commented channels are ignored by ZapLink and do not appear in the Jellyfin M3U
playlist. Answer **Yes** to the weak-channel prompt to keep every discovered
channel active regardless of the validation lock or 20 dB threshold.

## HTTP Endpoints
Base URL: `http://<host>:18392`

- `GET /playlist.m3u`: M3U playlist for Jellyfin/VLC.
- `GET /xmltv.xml`: XMLTV EPG data (Jellyfin compatible).
- `GET /stream/<channel-id>.ts`: MPEG-TS live stream (for example, `/stream/15.1.ts`).
- `GET /stream/<channel-id>.mkv`: Matroska live stream when selected by the output profile.
- `HEAD /stream/<channel-id>.<ext>`: Validate a stream URL and inspect its media type without acquiring a tuner.

The generated playlist always uses a container-appropriate extension. Legacy
extensionless URLs such as `/stream/15.1` remain supported and resolve to the
same default MPEG-TS profile.

Normally the channel ID is its virtual channel number. If multiple received
stations advertise the same number, the playlist and XMLTV feed use a stable
frequency/service-qualified ID such as `15.1-581000000-3`. Requests using an
ambiguous bare number are rejected instead of silently tuning the first match.

### Streaming Parameters
All parameters are optional and can be appended to stream URLs or the playlist
URL. Names are case-sensitive. Unknown, duplicate, empty, malformed percent-
encoded, and control-character-containing parameters receive HTTP 400.

| Parameter | Values | Default | Description |
|-----------|--------|---------|-------------|
| `backend` | `software`, `qsv`, `nvenc`, `vaapi` | `software` | Hardware acceleration backend |
| `codec` | `copy`, `h264`, `hevc`, `av1` | `copy` | Video codec (copy = passthrough) |
| `bitrate` | Integer (kbps) | Auto | Target video bitrate (e.g., `6000`) |
| `audio` | `2`, `6`, `5.1` | `2` | Audio channels (stereo or 5.1 surround) |
| `container` | `mpegts`, `ts`, `matroska`, `mkv` | Auto | Output container; software AV1 requires Matroska |
| `latency` | `low`, `balanced`, `robust` | `balanced` | Bounded probe, buffering, keyframe, and warm-session policy |

The URL suffix and `container` parameter must agree. Invalid combinations are
rejected with HTTP 400 rather than returning content under a misleading media
type. Software AV1 retains its established Matroska output; other profiles use
MPEG-TS unless explicitly configured otherwise.

### Latency Profiles

| Profile | Input analysis | Probe size | Forced transcode keyframes | Idle session lifetime | Use case |
|---------|----------------|------------|----------------------------|-----------------------|----------|
| `low` | 0.5 s | 1 MB | 1 s | 5 s | Interactive channel changes on clean local broadcasts |
| `balanced` | 1 s | 5 MB | 2 s | 5 s | Default; good detection with short startup |
| `robust` | 3 s | 20 MB | 3 s | 10 s | Sparse, damaged, or slow-to-identify transports |

The low profile also enables FFmpeg's reduced-analysis buffering. All MPEG-TS
profiles flush output packets promptly and repeat PAT/PMT tables. If a low
profile loses audio or reports incomplete codec parameters, use `balanced` or
`robust` rather than increasing arbitrary FFmpeg arguments.

### Reusable Stream Sessions

ZapLink normalizes the effective channel, container, codec, backend, bitrate,
audio, and latency values into a stream-session key. Requests with the same
effective settings share a single tuner and FFmpeg/remux producer even when
their query parameters appear in a different order. Each client has an
independent read cursor, so a slow or disconnected subscriber cannot block the
producer indefinitely.

After the last MPEG-TS subscriber disconnects, ZapLink continues draining the
producer for the latency profile's short idle lifetime. A media probe followed
by playback can therefore reuse the tuned stream instead of consuming another
tuner and restarting FFmpeg. A different transcoding profile still requires a
separate producer. Matroska is not joined in progress because new subscribers
would not receive its required initial container header.

For `codec=copy`, backend, bitrate, and audio settings cannot affect the copied
transport and are canonicalized away. Compatibility URLs that differ only in
those irrelevant values intentionally share the same passthrough session.

### Streaming Modes

**Passthrough Mode (Default)**
When no `codec` is specified (or `codec=copy`), `dvbv5-zap` records only the
requested service's video and audio PIDs and adds PAT/PMT tables. This prevents
another subchannel on the same RF multiplex from being selected accidentally.
FFmpeg uses stream copy, so video and audio are not re-encoded. This provides:
- Minimal CPU usage for video processing
- Original video/audio quality preserved
- Lowest possible latency

**Transcoding Mode**
When a codec is specified (`h264`, `hevc`, `av1`), FFmpeg processes the stream with:
- Low-latency encoder presets (ultrafast/zerolatency)
- Deinterlacing filters
- AAC audio encoding (stereo by default, or 5.1 with `audio=6`)
- MPEG-TS muxer optimizations for live streaming

### Examples

1. **Passthrough (Default)** - zero CPU, original quality:
   ```sh
   curl "http://localhost:18392/stream/15.1.ts" > out.ts
   ```

2. **Software Transcode to H.264** (stereo audio):
   ```sh
   curl "http://localhost:18392/stream/15.1.ts?codec=h264" > out.ts
   ```

3. **Hardware Transcode (Intel QSV) to H.264 at 6Mbps**:
   ```sh
   curl "http://localhost:18392/stream/15.1.ts?backend=qsv&codec=h264&bitrate=6000&latency=low" > out.ts
   ```

4. **Hardware Transcode (VA-API) to HEVC with 5.1 audio**:
   ```sh
   curl "http://localhost:18392/stream/15.1.ts?backend=vaapi&codec=hevc&audio=6" > out.ts
   ```

5. **NVIDIA NVENC to H.264**:
   ```sh
   curl "http://localhost:18392/stream/15.1.ts?backend=nvenc&codec=h264" > out.ts
   ```

## Jellyfin Setup
To use ZapLink with Jellyfin:
1. Go to **Dashboard** > **Live TV**.
2. Add **Tuner Device** (Select **M3U Tuner**):
   - **File or URL**: `http://<ip>:18392/playlist.m3u`
   - *Tip: Append parameters to apply them to all channels:*
     `http://<ip>:18392/playlist.m3u?backend=qsv&codec=h264`
3. Add **TV Guide Data Provider** (Select **XMLTV**):
   - **File or URL**: `http://<ip>:18392/xmltv.xml`
4. Save and click **Refresh Guide Data**.

> [!IMPORTANT]
> **Upgrading from Extensionless URLs (`Unable to find host to play channel` error):**
> Jellyfin generates its internal channel keys and `MediaSource.Id` by MD5-hashing the exact stream URL found in `playlist.m3u`.
> When upgrading from extensionless stream URLs (`/stream/15.1`) to extension-based URLs (`/stream/15.1.ts`), existing channel mappings, series recording timers, and client caches in Jellyfin will still reference the old hash. Attempting playback before refreshing will trigger:
> ```text
> MediaBrowser.Controller.LiveTv.LiveTvConflictException: Unable to find host to play channel
> ```
> Or in the Jellyfin client UI: `"This Item can not be played"` / `"Playback Error"`.
>
> **Required Migration Steps:**
> 1. In Jellyfin, go to **Administration > Dashboard > Live TV**.
> 2. Under **Tuner Devices**, click on your M3U Tuner and click **Save** (or click the three dots `...` to refresh).
> 3. Under **TV Guide Data Providers**, click the three dots `...` next to your XMLTV provider and select **Refresh Guide Data**.
> 4. **Hard-refresh your browser/client** (`Ctrl + F5` or `Cmd + Shift + R`) or restart the Jellyfin app so the client loads the new active channel IDs.
> 5. If you have existing series recording timers created under old channel hashes, verify and re-save them under the updated guide channels.

To verify the live transport independently of Jellyfin, request a known active
channel for a bounded interval:

```sh
curl --max-time 8 --output /dev/null \
  --write-out 'HTTP %{http_code}, %{size_download} bytes\n' \
  http://127.0.0.1:18392/stream/15.1.ts
```

A live stream normally ends this test with curl timeout status 28; HTTP 200 and
a nonzero byte count confirm that transport data was received. If zero bytes
are returned, inspect `journalctl -u zaplink` for tuner discovery, no-tuner, or
stream-stall messages.

Check a profile without opening a tuner:

```sh
curl --head 'http://127.0.0.1:18392/stream/15.1.ts?latency=low'
```

The response should report `Content-Type: video/mp2t` and
`X-Zaplink-Latency: low`. Session lifecycle logs use component `SESSION` and
distinguish a newly created producer from a reused one. If identical requests
create separate producers, confirm that every media-affecting option is equal;
different bitrate, audio, codec, backend, container, or latency values are
intentionally isolated when they affect the selected mode. In passthrough mode,
backend, bitrate, and audio are ignored and therefore do not isolate sessions.

## Verification

Run the build and regression suite covering channel identity, tuner lease
preemption without lock-held waits, concurrent EPG database transactions,
strict profile parsing, concurrent shared delivery, session isolation and
exhaustion, ring-buffer overrun, linger cleanup, producer failures, FFmpeg
argument ordering, hermetic HTTP GET/HEAD behavior, and ATSC A/65 Huffman decoding:

```sh
make test
```

**Note (Windows users):** When using URLs with `&` in Command Prompt, wrap the URL in quotes:
```cmd
mpv "http://falcon:18392/playlist.m3u?backend=qsv&codec=h264"
```

## Data Files
- `/var/lib/zaplink/channels.conf`: Channel configuration used by the packaged service.
- `/var/lib/zaplink/epg.db`: SQLite EPG database used by the packaged service.
- `huffman.bin`: Required for decoding certain ATSC EPG strings.

## Safe Upgrade and Rollback

Before replacing a running installation, confirm that no stream, recording, or
EPG scan is active. Save both the binary and the effective systemd unit with
root-only ownership; a binary-only rollback is insufficient when service flags
or writable paths change. Stop the service, migrate an existing
`/opt/zaplink/epg.db` and `/opt/zaplink/channels.conf` into `/var/lib/zaplink`
if required, install the new binary and unit, run `systemctl daemon-reload`,
and start the service. Verify
ownership, endpoints, strict parameter rejection, and logs before deleting any
rollback files.

If verification fails, stop the service, restore both saved files, reload
systemd, restart ZapLink, and verify the restored endpoints and process tree.
Rollback artifacts should remain `root:root` and mode `0755` for binaries or
`0644` for units.

## Notes
- **Series Recording**: The generated XMLTV includes special tags (`<episode-num>`, `<category>Series</category>`) to ensure Jellyfin recognizes recurring shows.
- **VHF Skipping**: During scan, you can choose to skip VHF-LO/HI (RF 2-13) if you only have a UHF antenna.
- **Weak Channels**: The scanner asks whether to retain multiplexes below 20 dB C/N. By default their complete configuration blocks are preserved as comments so they are visible for later review but unavailable to Jellyfin.

## License
No license specified.
