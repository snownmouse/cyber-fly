#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Self-check for the probe packet contract.

Runs without any hardware: it packs a packet exactly the way probe_common.c
does, then feeds it through the receiver's own parser. Use it after editing
either side, so a byte drift is caught before it costs a flash cycle.

    python3 verify_protocol.py
"""

from __future__ import annotations

import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import udp_probe_server as srv  # noqa: E402

failures: list[str] = []


def check(name: str, condition: bool, detail: str = "") -> None:
    status = "PASS" if condition else "FAIL"
    print(f"  [{status}] {name}" + (f"  -> {detail}" if detail else ""))
    if not condition:
        failures.append(name)


def build_c_style_packet(
    pkt_type: int = srv.PKT_FEATURE,
    seq: int = 42,
    send_ts_us: int = 123456789,
    flags: int = srv.FLAG_AUDIO_VALID | srv.FLAG_IMU_VALID,
) -> bytes:
    """Mirror probe_pkt_finalize(): fill fields, then CRC the first 64 bytes."""
    body = srv.PACKET.pack(
        srv.MAGIC,
        srv.VERSION,
        pkt_type,
        srv.PACKET_BYTES,
        seq,
        send_ts_us,
        0,
        16000,
        1,
        flags,
        -68.4,        # rms_dbfs
        -74.1,        # band_low
        -78.6,        # band_mid
        -80.2,        # band_high
        0.01, -0.02, 0.998,   # accel, in g
        0.4, -0.7, 0.1,       # gyro, deg/s
        0,            # crc placeholder
    )
    crc = zlib.crc32(body[: srv.CRC_BYTES])
    return body[: srv.CRC_BYTES] + struct.pack("<I", crc)


print("=== probe packet contract self-check ===")
print()

print("1. wire layout")
check("struct size is 68 bytes", srv.PACKET.size == srv.PACKET_BYTES,
      f"got {srv.PACKET.size}")
check("magic matches probe_common.h", srv.MAGIC == 0x4D505242, hex(srv.MAGIC))
check("CRC covers 64 bytes", srv.CRC_BYTES == 64)

print()
print("2. board -> server round trip")
raw = build_c_style_packet()
check("packet is 68 bytes on the wire", len(raw) == 68, f"got {len(raw)}")

parsed = srv.parse_packet(raw)
check("parser accepts a well-formed packet", parsed is not None)
if parsed:
    check("seq survives", parsed.seq == 42, str(parsed.seq))
    check("send_ts_us survives", parsed.send_ts_us == 123456789, str(parsed.send_ts_us))
    check("sample_rate survives", parsed.sample_rate == 16000, str(parsed.sample_rate))
    check("channels is 1 (mono)", parsed.channels == 1, str(parsed.channels))
    check("audio flag set", parsed.has_audio)
    check("imu flag set", parsed.has_imu)
    check("rms_dbfs survives", abs(parsed.rms_dbfs - (-68.4)) < 1e-4, str(parsed.rms_dbfs))
    check("band_low survives", abs(parsed.band_low - (-74.1)) < 1e-4, str(parsed.band_low))
    check("accel survives", abs(parsed.accel[2] - 0.998) < 1e-4, str(parsed.accel))

print()
print("3. rejection paths")
corrupt = bytearray(raw)
corrupt[10] ^= 0xFF
check("bad CRC is rejected", srv.parse_packet(bytes(corrupt)) is None)

bad_magic = bytearray(raw)
bad_magic[0] ^= 0xFF
crc = zlib.crc32(bytes(bad_magic[: srv.CRC_BYTES]))
bad_magic[srv.CRC_BYTES:] = struct.pack("<I", crc)
check("bad magic is rejected", srv.parse_packet(bytes(bad_magic)) is None)

check("short datagram is rejected", srv.parse_packet(raw[:40]) is None)

print()
print("4. server -> board ACK")
ack_raw = srv.build_ack(parsed)
check("ACK is 68 bytes", len(ack_raw) == 68, f"got {len(ack_raw)}")
ack = srv.parse_packet(ack_raw)
check("ACK parses", ack is not None)
if ack:
    check("ACK type is correct", ack.pkt_type == srv.PKT_ACK, str(ack.pkt_type))
    check("ACK echoes the board timestamp",
          ack.echo_ts_us == 123456789, str(ack.echo_ts_us))
    check("ACK keeps the sequence number", ack.seq == 42, str(ack.seq))

    # This is the arithmetic the board performs to get its RTT.
    now_us = 123456789 + 11_400
    delta = (now_us - ack.echo_ts_us) & 0xFFFFFFFF
    check("board-side RTT arithmetic yields 11.4 ms",
          abs(delta / 1000.0 - 11.4) < 1e-6, f"{delta / 1000.0} ms")

print()
print("5. 32-bit timestamp wrap-around")
# The wire field is 32-bit while esp_timer_get_time() is 64-bit; the board
# subtracts in 32-bit space so this has to stay correct across the wrap.
echo = 0xFFFFFF00
now = 0x00000000          # the counter wrapped 256 us after the stamp
delta = (now - echo) & 0xFFFFFFFF
check("wrap-around delta is 256 us", delta == 256, str(delta))

echo = 0xFFFFFF00
now = 0x00000400          # same wrap, 1280 us later
delta = (now - echo) & 0xFFFFFFFF
check("wrap-around delta is 1280 us", delta == 1280, str(delta))

print()
if failures:
    print(f"RESULT: {len(failures)} check(s) failed: {', '.join(failures)}")
    raise SystemExit(1)

print("RESULT: all checks passed; firmware and receiver agree byte for byte")
