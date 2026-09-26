#!/usr/bin/env python3
"""Check the no-hardware HTTP server's descriptors before subprocess exec."""
import os
import pathlib
import socket
import subprocess
import tempfile
import time

root = pathlib.Path(__file__).resolve().parent.parent
binary = root / "build/zaplink_no_tuner_test"
with tempfile.TemporaryDirectory(prefix="zaplink-fd-test-") as directory:
    config = pathlib.Path(directory) / "channels.conf"
    config.write_text("[TEST]\nVCHANNEL = 15.1\nSERVICE_ID = 3\nFREQUENCY = 581000000\n")
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    env = dict(os.environ, ZAPLINK_CHANNELS_PATH=str(config))
    log = open(pathlib.Path(directory) / "server.log", "w")
    server = subprocess.Popen([str(binary), "-n", "-p", str(port)], env=env,
                              stdout=log, stderr=log)
    clients = []
    try:
        for _ in range(100):
            try:
                client = socket.create_connection(("127.0.0.1", port), timeout=1)
                clients.append(client)
                break
            except OSError:
                assert server.poll() is None, "server exited before binding"
                time.sleep(.02)
        else:
            raise AssertionError("server did not bind")
        # Keep one accepted client incomplete so it is still owned by epoll.
        client.sendall(b"GET /missing HTTP/1.1\r\n")
        time.sleep(.05)
        fd_dir = pathlib.Path(f"/proc/{server.pid}/fd")
        kinds = []
        for fd in fd_dir.iterdir():
            if int(fd.name) < 3:
                continue
            try:
                kind = os.readlink(fd)
                flags = int((pathlib.Path(f"/proc/{server.pid}/fdinfo") / fd.name)
                            .read_text().split("flags:\t", 1)[1].splitlines()[0], 8)
            except (FileNotFoundError, ProcessLookupError):
                continue
            if kind.startswith(("socket:", "pipe:", "anon_inode:[eventpoll]")):
                kinds.append(kind.split(":", 1)[0])
                assert flags & os.O_CLOEXEC, f"{kind} on fd {fd.name} lacks CLOEXEC"
        assert kinds.count("socket") >= 2, kinds
        assert kinds.count("pipe") >= 2, kinds
        assert kinds.count("anon_inode") >= 1, kinds
        client.sendall(b"Host: localhost\r\n\r\n")
        client.settimeout(1)
        response = bytearray()
        while True:
            chunk = client.recv(8192)
            if not chunk:
                break
            response.extend(chunk)
        assert b"404 Not Found" in response, response
        print("HTTP child descriptor isolation tests: OK")
    finally:
        for client in clients:
            client.close()
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
        log.close()
