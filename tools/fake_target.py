#!/usr/bin/env python3
"""Stand-in for a J-Link RTT telnet server, for trying the plugin without
hardware.

Streams records in the layout of examples/telemetry_map.json: a text banner
first (as J-Link does), then binary records at a fixed rate. Every 10 s the
sequence counter restarts, as if the target had rebooted.

Usage: tools/fake_target.py [--port 19021] [--rate 1000]
"""

import argparse
import math
import socket
import struct
import time

MAGIC = 0xC3A55A01
RECORD = struct.Struct("<IIfffhB")  # 23 bytes, see examples/firmware/telemetry.c
BANNER = b"SEGGER J-Link (fake_target.py) - Real time terminal output\r\n"


def record(seq: int, rate: float) -> bytes:
    t = seq / rate
    speed = 1500.0 * (1.0 if (t % 4.0) < 2.0 else 0.5)
    current_a = 2.0 * math.sin(2 * math.pi * 50 * t)
    current_b = 2.0 * math.sin(2 * math.pi * 50 * t - 2 * math.pi / 3)
    temp_cdeg = int(2500 + 10 * t)
    state = 1 if (t % 4.0) < 2.0 else 2
    return RECORD.pack(MAGIC, seq, current_a, current_b, speed, temp_cdeg, state)


def serve(conn: socket.socket, rate: float) -> None:
    conn.sendall(BANNER)
    conn.settimeout(0.2)
    try:
        print("config string:", conn.recv(64).decode(errors="replace"))
    except socket.timeout:
        pass

    batch = max(1, int(rate / 100))  # send in 10 ms chunks, like a real probe
    seq = 0
    start = time.monotonic()
    while True:
        chunk = b"".join(record(seq + i, rate) for i in range(batch))
        conn.sendall(chunk)
        seq += batch
        if seq >= 10 * rate:
            seq = 0
            start = time.monotonic()
        delay = start + seq / rate - time.monotonic()
        if delay > 0:
            time.sleep(delay)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=19021)
    parser.add_argument("--rate", type=float, default=1000.0, help="records per second")
    args = parser.parse_args()

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", args.port))
    server.listen(1)
    print(f"listening on 127.0.0.1:{args.port}")
    while True:
        conn, _ = server.accept()
        print("client connected")
        try:
            serve(conn, args.rate)
        except (BrokenPipeError, ConnectionResetError):
            print("client disconnected")
        finally:
            conn.close()


if __name__ == "__main__":
    main()
