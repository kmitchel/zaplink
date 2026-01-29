# ZapLink Hardening Walkthrough

This document details the security and robustness improvements applied to the ZapLink transcoding pipeline.

## 1. Objectives
-   **Eliminate Shell Injection**: Remove all usage of `/bin/sh` or `system()`.
-   **Prevent Buffer Overflows**: Enforce strict argument limits for process execution.
-   **Harden Network Input**: Protect against DoS attacks via malformed HTTP requests.

## 2. Implementation Details

### Process Execution Refactor (`src/transcode.c`)
The pipeline `dvbv5-zap | ffmpeg` is now constructed using direct system calls:
-   **`fork()` & `pipe()`**: Processes are spawned individually with manual pipe management.
-   **`execvp()`**: Binaries are executed directly, bypassing the shell.

### Argument Safety Strategy
To prevent command truncation or buffer overflows, a "sticky error" pattern is used for building the argument list:
-   **`add_arg` Helper**: Accepts an error pointer (`int *err`).
-   **Propagation**: If an overflow occurs (limit 128), `add_arg` sets the error flag and logs an error. Subsequent calls no-op.
-   **Pre-Execution Check**: Before calling `execvp`, the error flag is checked. If set, the process aborts immediately with `_exit(1)`. This guarantees that partial or corrupted commands are **never** executed.

### HTTP Server Hardening (`src/http_server.c`)
-   **Buffered Reading**: Handles fragmented TCP packets robustly.
-   **Size Limit**: Enforces a 4KB maximum for HTTP headers.
-   **Deferred Response**: Waits for valid stream data before sending `200 OK`.

## 3. Verification
-   **Passthrough Mode**: `ffmpeg -c copy` verified working.
-   **Transcoding**: Hardware acceleration (QSV/NVENC/VAAPI) arguments verified.
-   **Process Tree**: Confirmed no `sh` processes in the hierarchy.
