# ZapLink Hardening Walkthrough

This document details the security and robustness improvements applied to the ZapLink transcoding pipeline.

## 1. Objectives
-   **Eliminate Shell Injection**: Remove all usage of `/bin/sh` or `system()`.
-   **Prevent Buffer Overflows**: Enforce strict argument limits for process execution.
-   **Harden Network Input**: Protect against DoS attacks via malformed HTTP requests.
-   **Handle Device Startup Races**: Wait for usable DVB frontend nodes before tuner discovery.
-   **Protect Stream Reliability**: Exclude marginal multiplexes from the generated playlist by default.

## 2. Implementation Details

### Process Execution Refactor (`src/transcode.c`)
The pipeline `dvbv5-zap | ffmpeg` is constructed without a shell:
-   **`posix_spawnp()` & `pipe2()`**: Both children are launched with explicit file actions and a shared process group. Argument and device discovery occurs in the parent, so no allocation-dependent work occurs in a post-`fork()` child.
-   **Launch diagnostics**: `posix_spawnp()` reports executable and setup failures directly. A producer becomes ready only after media bytes arrive; premature pipeline exit is reported to subscribers.
-   **Transport Output**: `dvbv5-zap` runs with `-p -o -`. Lowercase `-p` selects the requested service PIDs, adds PAT/PMT, implies record mode, and writes MPEG-TS to the pipeline. Uppercase `-P` must not be used here because it emits every program on the multiplex and allows FFmpeg to select the first subchannel.
-   **Low-Latency Output**: MPEG-TS output repeats PAT/PMT, marks the initial discontinuity, flushes packets promptly, and uses time-based forced keyframes for transcoded video.

### Stream Contracts and Profiles (`src/stream_config.c`)

-   Generated URLs carry `.ts` or `.mkv` according to the actual muxer and MIME type. Existing extensionless URLs remain valid.
-   `HEAD` validates the channel and normalized profile but never acquires a tuner.
-   `low`, `balanced`, and `robust` profiles select bounded analysis duration, probe size, transcode keyframe interval, input buffering, and idle-session lifetime.
-   Explicit URL suffixes and `container` parameters must agree. Software AV1 remains Matroska and invalid combinations fail before process creation.

### Reusable Delivery (`src/stream_session.c`)

-   A session key compares normalized channel, backend, codec, container, latency, bitrate, and audio values rather than raw query-string order.
-   Identical MPEG-TS subscribers receive one producer through a bounded 4 MB broadcast ring. A subscriber that falls behind the retained window is disconnected instead of blocking every viewer.
-   The producer remains alive for a short profile-dependent grace period after its final subscriber leaves. Probe-to-playback reconnects attach to that producer without retuning.
-   Producer startup failure wakes every waiter, releases the session slot, and permits a later request to recover. Shutdown stops producers and waits for tuner/process cleanup.
-   Matroska sessions are intentionally not joined in progress because their initial container header cannot be reconstructed safely for a late subscriber.

### Argument Safety Strategy
To prevent command truncation or buffer overflows, a "sticky error" pattern is used for building the argument list:
-   **`add_arg` Helper**: Accepts an error pointer (`int *err`).
-   **Propagation**: If an overflow occurs (limit 128), `add_arg` sets the error flag and subsequent calls no-op.
-   **Pre-Execution Check**: The error flag is checked in the parent before either process is launched, so partial commands are never executed.

### HTTP Server Hardening (`src/http_server.c`)
-   **Buffered Reading**: Handles fragmented TCP packets robustly.
-   **Size Limit**: Enforces a 4KB maximum for HTTP headers.
-   **Header Deadline**: Incomplete requests expire after 10 seconds and pending header connections are capped.
-   **Input Validation**: A single parser rejects unknown or duplicate names, empty values, malformed percent escapes, decoded control characters, invalid ranges, and container conflicts before dispatch.
-   **Side-Effect-Free HEAD**: Stream metadata can be inspected without creating a pipeline or reserving DVB hardware.
-   **Deferred Response**: Waits for valid stream data before sending `200 OK`.
-   **Bounded Pipelines**: Stream startup and later no-data stalls terminate after 15 seconds with bounded process-group cleanup.

### DVB Readiness (`src/tuner.c`)

-   Startup polls `/dev/dvb/adapter*/frontend*` every 250 ms for up to 30 seconds.
-   A frontend is usable only when the ZapLink process can both read and write it.
-   Tuner discovery resets its pool and excludes adapter directories without a usable frontend.
-   A timeout is nonfatal: the HTTP service still starts, logs that streaming is unavailable, and can be restarted after the device appears.

### Scan Quality Control (`src/scanner.c`)

-   The scanner validates every unique RF multiplex after `dvbv5-scan` finishes.
-   It invokes `dvbv5-zap` against a representative service and averages the reported signal and C/N samples.
-   No-lock multiplexes and multiplexes below 20 dB C/N are classified as weak.
-   The interactive prompt can retain weak channels. The default comments every service block on a weak multiplex while preserving its measurements for review.
-   Configuration replacement uses a temporary file, flushes it to disk, and atomically renames it over `channels.conf`.
-   Scanner worker status and output are validated; an empty or failed scan never replaces the active configuration.
-   Setup requires a TTY, so service startup cannot loop on end-of-file prompts.

### Tuner Ownership and EPG Shutdown (`src/tuner.c`, `src/epg.c`)

-   Every tuner acquisition receives a generation. A preempted EPG worker cannot release or overwrite the stream lease that replaced it.
-   Ownership transitions occur under the tuner mutex, but all signals and waits occur after it is released. Termination has bounded TERM and KILL phases; a detached reaper keeps an unresponsive process quarantined until it exits.
-   The orchestrator is joinable, successfully created workers are tracked, and shutdown interrupts EPG children before joining all threads and closing SQLite.
-   Complete SQLite reads and write transactions are serialized on the shared connection.
-   PSIP sections are rejected when their declared length or MPEG-2 CRC is invalid, and transport continuity gaps reset partial section assembly.

### Channel Identity (`src/channels.c`)

-   Duplicate virtual channel numbers receive frequency/service-qualified identifiers shared by M3U, XMLTV, JSON, and stream lookup.
-   Ambiguous bare channel numbers are rejected instead of selecting the first configuration entry.

### Logging (`include/log.h`)

-   Logs contain `timestamp`, `level`, `component`, and `message` fields on one line.
-   Syslog priority prefixes let journald retain severity without a libsystemd dependency.
-   Terminal color sequences are never emitted.

## 3. Verification
-   **Passthrough Mode**: The production endpoint returned HTTP 200 and transferred the requested service using `dvbv5-zap -p` with FFmpeg stream copy.
-   **Transcoding**: Hardware acceleration (QSV/NVENC/VAAPI) arguments verified.
-   **Process Tree**: Confirmed no `sh` processes in the hierarchy.
-   **Startup Readiness**: Verified immediate discovery with two frontends and the bounded 30-second no-device timeout path.
-   **Signal Filtering**: Verified both default exclusion and explicit retention against a 49-channel configuration; the filtered file loaded only active, uncommented services.
-   **Regression Tests**: `make test` includes hermetic HTTP tests and verifies duplicate IDs, stale and concurrent tuner lease handling, strict query parsing, typed and compatibility URLs, profile normalization, concurrent reuse, isolation, capacity, overrun, linger cleanup, process launch errors, listener failure, and producer recovery.
-   **Noninteractive Scanner**: Closed stdin exits with status 1 and a structured error without modifying `channels.conf`.
-   **Graceful Streaming Shutdown**: An active transport stream terminates and releases its tuner within one second of SIGTERM in the hardware integration check.
