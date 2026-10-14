#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
UDP receiver for the ESP-Mosaico hardware probe (task A).

Pairs with tests/firmware/hw_probe. It validates and counts the packets the
board sends, replies with an ACK that echoes the board's timestamp so the board
can compute a real round-trip time, and prints per-session statistics.

Sessions are detected automatically: burst packets and feature frames are
counted separately, and a session is reported when the packet type changes or
when you stop the script with Ctrl-C.

Usage:
    python3 udp_probe_server.py                 # listen on 0.0.0.0:33333
    python3 udp_probe_server.py --port 33333
    python3 udp_probe_server.py --quiet         # suppress per-packet lines
"""

from __future__ import annotations

import argparse
import socket
import statistics
import struct
import sys
import time
import zlib
from dataclasses import dataclass, field

MAGIC = 0x4D505242  # "MPRB"
VERSION = 1
PACKET_BYTES = 68
CRC_BYTES = 64

PKT_BURST = 1
PKT_FEATURE = 2
PKT_ACK = 3

TYPE_NAMES = {PKT_BURST: "burst", PKT_FEATURE: "feature", PKT_ACK: "ack"}

FLAG_AUDIO_VALID = 1 << 0
FLAG_IMU_VALID = 1 << 1

# Little-endian, no padding: matches the packed C struct byte for byte.
PACKET = struct.Struct("<IBBHIIIHBBffff3f3fI")

assert PACKET.size == PACKET_BYTES, "packet layout drifted from probe_common.h"


@dataclass
class Packet:
    magic: int
    version: int
    pkt_type: int
    payload_len: int
    seq: int
    send_ts_us: int
    echo_ts_us: int
    sample_rate: int
    channels: int
    flags: int
    rms_dbfs: float
    band_low: float
    band_mid: float
    band_high: float
    accel: tuple[float, float, float]
    gyro: tuple[float, float, float]
    crc32: int

    @property
    def has_audio(self) -> bool:
        return bool(self.flags & FLAG_AUDIO_VALID)

    @property
    def has_imu(self) -> bool:
        return bool(self.flags & FLAG_IMU_VALID)


def parse_packet(data: bytes) -> Packet | None:
    """Return a Packet, or None when the datagram is not a valid probe packet."""
    if len(data) != PACKET_BYTES:
        return None

    # '<...3f3f...' unpacks each float component separately, so accel and gyro
    # have to be regrouped into tuples before they reach the dataclass.
    (magic, version, pkt_type, payload_len, seq, send_ts_us, echo_ts_us,
     sample_rate, channels, flags,
     rms_dbfs, band_low, band_mid, band_high,
     ax, ay, az, gx, gy, gz,
     crc32) = PACKET.unpack(data)

    if magic != MAGIC or version != VERSION:
        return None
    if payload_len != PACKET_BYTES:
        return None
    if zlib.crc32(data[:CRC_BYTES]) != crc32:
        return None

    return Packet(
        magic=magic, version=version, pkt_type=pkt_type, payload_len=payload_len,
        seq=seq, send_ts_us=send_ts_us, echo_ts_us=echo_ts_us,
        sample_rate=sample_rate, channels=channels, flags=flags,
        rms_dbfs=rms_dbfs, band_low=band_low, band_mid=band_mid, band_high=band_high,
        accel=(ax, ay, az), gyro=(gx, gy, gz), crc32=crc32,
    )


def build_ack(pkt: Packet) -> bytes:
    """Echo the board timestamp so it can compute the round trip."""
    body = PACKET.pack(
        MAGIC,
        VERSION,
        PKT_ACK,
        PACKET_BYTES,
        pkt.seq,
        0,                # the server never writes into the board's clock
        pkt.send_ts_us,   # the echo is what makes the RTT measurable on the board
        pkt.sample_rate,
        pkt.channels,
        0,                # an ACK carries no feature validity flags
        0.0, 0.0, 0.0, 0.0,   # rms + three band energies
        0.0, 0.0, 0.0,        # accel
        0.0, 0.0, 0.0,        # gyro
        0,                # CRC placeholder, filled below
    )
    crc = zlib.crc32(body[:CRC_BYTES])
    return body[:CRC_BYTES] + struct.pack("<I", crc)


@dataclass
class Session:
    kind: int
    started: float
    received: int = 0
    duplicates: int = 0
    out_of_order: int = 0
    first_seq: int | None = None
    last_seq: int = -1
    max_seq: int = -1
    arrivals: list[float] = field(default_factory=list)
    audio_frames: int = 0
    imu_frames: int = 0
    band_low: list[float] = field(default_factory=list)
    band_mid: list[float] = field(default_factory=list)
    band_high: list[float] = field(default_factory=list)
    rms: list[float] = field(default_factory=list)
    accel_mag: list[float] = field(default_factory=list)
    seen: set[int] = field(default_factory=set)

    def add(self, pkt: Packet, now: float) -> None:
        self.received += 1

        if pkt.seq in self.seen:
            self.duplicates += 1
        else:
            self.seen.add(pkt.seq)

        if self.first_seq is None:
            self.first_seq = pkt.seq
        if pkt.seq < self.max_seq:
            self.out_of_order += 1
        if pkt.seq > self.max_seq:
            self.max_seq = pkt.seq
        self.last_seq = pkt.seq

        # Raw receive times; inter-arrival stats are derived in report().
        self.arrivals.append(now)

        if pkt.has_audio:
            self.audio_frames += 1
            self.band_low.append(pkt.band_low)
            self.band_mid.append(pkt.band_mid)
            self.band_high.append(pkt.band_high)
            self.rms.append(pkt.rms_dbfs)

        if pkt.has_imu:
            self.imu_frames += 1
            ax, ay, az = pkt.accel
            self.accel_mag.append((ax * ax + ay * ay + az * az) ** 0.5)

    @property
    def expected(self) -> int:
        if self.first_seq is None:
            return 0
        return self.max_seq - self.first_seq + 1

    @property
    def lost(self) -> int:
        return max(0, self.expected - len(self.seen))

    @property
    def loss_pct(self) -> float:
        return (self.lost * 100.0 / self.expected) if self.expected else 0.0

    def report(self) -> None:
        duration = max(1e-6, self.arrivals[-1] - self.arrivals[0]) if len(self.arrivals) > 1 else 0.0
        kind = TYPE_NAMES.get(self.kind, str(self.kind))

        print()
        print(f"================ SESSION: {kind} ================")
        print(f"  received         : {self.received} datagrams")
        print(f"  sequence range   : {self.first_seq}..{self.max_seq}  (expected {self.expected})")
        print(f"  loss             : {self.lost} packets ({self.loss_pct:.2f} %)")
        print(f"  out of order     : {self.out_of_order}")
        print(f"  duplicates       : {self.duplicates}")

        if duration > 0:
            rate = (self.received - 1) / duration
            print(f"  duration         : {duration:.2f} s  ({rate:.1f} pkt/s effective)")

        if len(self.arrivals) > 2:
            gaps = [(self.arrivals[i] - self.arrivals[i - 1]) * 1000.0
                    for i in range(1, len(self.arrivals))]
            gaps_sorted = sorted(gaps)
            p95 = gaps_sorted[int(len(gaps_sorted) * 0.95) - 1]
            print(f"  inter-arrival    : avg {statistics.mean(gaps):.2f} ms  "
                  f"p95 {p95:.2f} ms  max {max(gaps):.2f} ms  "
                  f"stdev {statistics.pstdev(gaps):.2f} ms")

        if self.audio_frames:
            print(f"  audio frames     : {self.audio_frames}/{self.received}")
            print(f"  rms dBFS         : avg {statistics.mean(self.rms):.1f}  "
                  f"min {min(self.rms):.1f}  max {max(self.rms):.1f}")
            print(f"  band <200 Hz     : avg {statistics.mean(self.band_low):.1f} dBFS")
            print(f"  band 200-500 Hz  : avg {statistics.mean(self.band_mid):.1f} dBFS")
            print(f"  band 500-2000 Hz : avg {statistics.mean(self.band_high):.1f} dBFS")

        if self.imu_frames:
            print(f"  imu frames       : {self.imu_frames}/{self.received}")
            print(f"  accel magnitude  : avg {statistics.mean(self.accel_mag):.3f} g  "
                  f"min {min(self.accel_mag):.3f}  max {max(self.accel_mag):.3f}")

        if self.received and self.lost == 0 and self.out_of_order == 0:
            print("  verdict          : clean link for this session")
        elif self.received:
            print("  verdict          : losses present, note the position of the dips")
        print("=" * 50)
        sys.stdout.flush()


def local_ip_toward(target: str) -> str:
    """Best-effort guess of the address the board should send to."""
    try:
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.settimeout(0.2)
        probe.connect((target, 9))
        addr = probe.getsockname()[0]
        probe.close()
        return addr
    except OSError:
        return "0.0.0.0"


def main() -> int:
    parser = argparse.ArgumentParser(description="ESP-Mosaico hw_probe UDP receiver")
    parser.add_argument("--host", default="0.0.0.0", help="bind address (default 0.0.0.0)")
    parser.add_argument("--port", type=int, default=33333, help="UDP port (default 33333)")
    parser.add_argument("--quiet", action="store_true", help="do not print every packet")
    parser.add_argument("--timeout", type=float, default=0.0,
                        help="exit after this many seconds of silence (0 = never)")
    parser.add_argument("--dump", metavar="FILE", default=None,
                        help="append every valid 68-byte feature/burst frame, raw, to FILE "
                             "(concatenated packets; feed it straight to stage1_loop.js --analyze)")
    args = parser.parse_args()

    dump_fp = None
    if args.dump:
        # 'ab' so repeated runs accumulate into one replayable stream instead of
        # truncating the previous take; delete the file yourself to start clean.
        dump_fp = open(args.dump, "ab")
        print(f"raw frame dump   : {args.dump}")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.host, args.port))
    sock.settimeout(1.0)

    guess = local_ip_toward("8.8.8.8")
    print(f"listening on {args.host}:{args.port}  (UDP)")
    print(f"put this in the firmware menuconfig if the board is on the same LAN: {guess}")
    print("waiting for probe packets; Ctrl-C prints the last session and exits")
    print()

    session: Session | None = None
    rejected = 0
    other = 0
    last_rx = time.monotonic()
    very_first = last_rx

    try:
        while True:
            try:
                data, addr = sock.recvfrom(2048)
            except socket.timeout:
                if args.timeout and (time.monotonic() - last_rx) > args.timeout:
                    print(f"\nno packets for {args.timeout:.0f} s, exiting")
                    break
                continue

            last_rx = time.monotonic()
            pkt = parse_packet(data)

            if pkt is None:
                rejected += 1
                if not args.quiet:
                    print(f"  ! rejected {len(data)}-byte datagram from {addr[0]}:{addr[1]} "
                          f"(bad magic, version, length or CRC)")
                continue

            if pkt.pkt_type == PKT_ACK:
                other += 1
                continue

            if session is None or session.kind != pkt.pkt_type:
                if session is not None:
                    session.report()
                session = Session(kind=pkt.pkt_type, started=last_rx)
                kind = TYPE_NAMES.get(pkt.pkt_type, str(pkt.pkt_type))
                print(f"--- new {kind} session from {addr[0]}:{addr[1]} ---")

            session.add(pkt, last_rx)
            sock.sendto(build_ack(pkt), addr)

            if dump_fp is not None:
                # Write the datagram verbatim: the board already built and CRC'd it,
                # and stage1_loop.js --analyze re-validates the same layout.
                dump_fp.write(data)
                if session.received % 20 == 0:      # ~1 s at 20 Hz
                    dump_fp.flush()

            if not args.quiet:
                extra = ""
                if pkt.has_audio:
                    extra = (f"  rms {pkt.rms_dbfs:7.1f} dBFS  "
                             f"bands {pkt.band_low:7.1f}/{pkt.band_mid:7.1f}/{pkt.band_high:7.1f}")
                elif pkt.has_imu:
                    ax, ay, az = pkt.accel
                    extra = f"  |a| {(ax * ax + ay * ay + az * az) ** 0.5:.3f} g"
                print(f"  #{pkt.seq:<6} {PACKET_BYTES} B  ts {pkt.send_ts_us}{extra}")

    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        if session is not None:
            session.report()
        sock.close()
        if dump_fp is not None:
            dump_fp.flush()
            dump_fp.close()
            print(f"raw frames written : {args.dump}")

    elapsed = time.monotonic() - very_first
    print(f"\nrejected datagrams : {rejected}")
    print(f"non-probe replies  : {other}")
    print(f"total run          : {elapsed:.1f} s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
