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
The pipeline `dvbv5-zap | ffmpeg` is now constructed using direct system calls:
-   **`fork()` & `pipe()`**: Processes are spawned individually with manual pipe management.
-   **`execvp()` / `execlp()`**: Binaries are executed directly, bypassing the shell.
-   **Transport Output**: `dvbv5-zap` runs with `-p -o -`. Lowercase `-p` selects the requested service PIDs, adds PAT/PMT, implies record mode, and writes MPEG-TS to the pipeline. Uppercase `-P` must not be used here because it emits every program on the multiplex and allows FFmpeg to select the first subchannel.

### Argument Safety Strategy
To prevent command truncation or buffer overflows, a "sticky error" pattern is used for building the argument list:
-   **`add_arg` Helper**: Accepts an error pointer (`int *err`).
-   **Propagation**: If an overflow occurs (limit 128), `add_arg` sets the error flag and logs an error. Subsequent calls no-op.
-   **Pre-Execution Check**: Before calling `execvp`, the error flag is checked. If set, the process aborts immediately with `_exit(1)`. This guarantees that partial or corrupted commands are **never** executed.

### HTTP Server Hardening (`src/http_server.c`)
-   **Buffered Reading**: Handles fragmented TCP packets robustly.
-   **Size Limit**: Enforces a 4KB maximum for HTTP headers.
-   **Deferred Response**: Waits for valid stream data before sending `200 OK`.

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

## 3. Verification
-   **Passthrough Mode**: The production endpoint returned HTTP 200 and transferred the requested service using `dvbv5-zap -p` with FFmpeg stream copy.
-   **Transcoding**: Hardware acceleration (QSV/NVENC/VAAPI) arguments verified.
-   **Process Tree**: Confirmed no `sh` processes in the hierarchy.
-   **Startup Readiness**: Verified immediate discovery with two frontends and the bounded 30-second no-device timeout path.
-   **Signal Filtering**: Verified both default exclusion and explicit retention against a 49-channel configuration; the filtered file loaded only active, uncommented services.
