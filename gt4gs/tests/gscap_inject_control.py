#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0+
"""Copies a GT4Recomp .gscap version 2 stream file and inserts synthetic CONTROL records (gt4-gs consumer tests).

Usage:
  gscap_inject_control.py <input.gscap> <output.gscap> --at FRAME:KIND[:ARG0[:ARG1[:ARG2]]] [--at ...] [--last FRAME]

--at inserts a CONTROL record (ControlPayload {u32 kind; u32 reserved; u64 args[3]}) right after the VSYNC record
whose `frame` field is FRAME, so it applies to the frames presented after that VSYNC. Several --at for the same frame
keep their order. --last stops after the VSYNC record of FRAME (a shorter file). Record sequence numbers are
renumbered 1, 2, 3, ...; the file header's protocol minor is raised to 3 when a kind 7 (display_aspect, protocol 1.3)
record is inserted. Only the Python standard library is used. The input file is never modified.
"""

import argparse
import struct
import sys

FILE_MAGIC = b"GSSTREAM"
FILE_HEADER_BYTES = 64
RECORD_HEADER = struct.Struct("<HHIQ")  # type, flags, payload_bytes, sequence
CONTROL_PAYLOAD = struct.Struct("<II3Q")  # kind, reserved, args[3]
RECORD_VSYNC = 3
RECORD_CONTROL = 6
CONTROL_KIND_DISPLAY_ASPECT = 7
COPY_CHUNK = 1 << 20


def aligned(size):
    return (size + 15) & ~15


def parse_at(text):
    parts = text.split(":")
    if not 2 <= len(parts) <= 5:
        raise argparse.ArgumentTypeError(f"--at {text!r} is not FRAME:KIND[:ARG0[:ARG1[:ARG2]]]")
    try:
        numbers = [int(part, 0) for part in parts]
    except ValueError:
        raise argparse.ArgumentTypeError(f"--at {text!r} has a value that is not an integer")
    frame, kind = numbers[0], numbers[1]
    args = (numbers[2:] + [0, 0, 0])[:3]
    if frame < 0 or not 0 <= kind < 2**32 or any(not 0 <= value < 2**64 for value in args):
        raise argparse.ArgumentTypeError(f"--at {text!r} is out of range")
    return frame, kind, args


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input")
    parser.add_argument("output")
    parser.add_argument("--at", action="append", type=parse_at, default=[], help="FRAME:KIND[:ARG0[:ARG1[:ARG2]]]")
    parser.add_argument("--last", type=int, default=None, help="stop after the VSYNC record of this frame")
    options = parser.parse_args()

    inserts = {}
    for frame, kind, args in options.at:
        inserts.setdefault(frame, []).append((kind, args))
    pending = set(inserts)

    with open(options.input, "rb") as source, open(options.output, "xb") as target:
        header = bytearray(source.read(FILE_HEADER_BYTES))
        if len(header) != FILE_HEADER_BYTES or header[:8] != FILE_MAGIC:
            sys.exit(f"{options.input} is not a version 2 GS stream file")
        file_version, major, minor = struct.unpack_from("<HHH", header, 8)
        if file_version != 2 or major != 1:
            sys.exit(f"{options.input}: file version {file_version}, protocol {major}.{minor} (need file version 2, protocol 1.x)")
        if any(kind == CONTROL_KIND_DISPLAY_ASPECT for entries in inserts.values() for kind, _ in entries):
            struct.pack_into("<H", header, 12, max(minor, 3))
        target.write(header)

        sequence = 0
        records = 0
        inserted = 0
        while True:
            raw = source.read(RECORD_HEADER.size)
            if not raw:
                break
            if len(raw) != RECORD_HEADER.size:
                sys.exit("truncated record header")
            record_type, flags, payload_bytes, _ = RECORD_HEADER.unpack(raw)
            sequence += 1
            records += 1
            target.write(RECORD_HEADER.pack(record_type, flags, payload_bytes, sequence))
            remaining = aligned(payload_bytes)
            first = b""
            while remaining:
                chunk = source.read(min(remaining, COPY_CHUNK))
                if not chunk:
                    sys.exit("truncated record payload")
                if not first:
                    first = chunk[:16]
                target.write(chunk)
                remaining -= len(chunk)
            if record_type != RECORD_VSYNC:
                continue
            frame = struct.unpack_from("<Q", first, 0)[0]
            for kind, args in inserts.get(frame, []):
                sequence += 1
                inserted += 1
                payload = CONTROL_PAYLOAD.pack(kind, 0, *args)
                target.write(RECORD_HEADER.pack(RECORD_CONTROL, 0, len(payload), sequence))
                target.write(payload + bytes(aligned(len(payload)) - len(payload)))
            pending.discard(frame)
            if options.last is not None and frame >= options.last:
                break

    if pending:
        sys.exit(f"no VSYNC record for frame(s) {sorted(pending)} in {options.input}")
    print(f"{options.output}: {records} records copied, {inserted} CONTROL records inserted")


if __name__ == "__main__":
    main()
