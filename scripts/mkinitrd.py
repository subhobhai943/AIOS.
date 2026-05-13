#!/usr/bin/env python3
"""
scripts/mkinitrd.py  —  AIOS initrd image builder

Usage:
    python3 scripts/mkinitrd.py  -o initrd.img  [file1 name1]  [file2 name2] ...

    Each pair is:  <host-path>  <ramdisk-path>
    E.g.:
        python3 scripts/mkinitrd.py -o initrd.img \
            assets/vocab.bin  /vocab.bin          \
            assets/config.bin /config.bin

Output format (see kernel/initrd.h for the full spec):
    [Header  16 bytes]
    [Entries N x 64 bytes]
    [File data -- packed]
"""

import struct
import sys
import os
import argparse

MAGIC    = 0x41524453   # 'ARDS'
VERSION  = 1
NAME_MAX = 48
ENTRY_SIZE = 64         # 48 (name) + 4 (offset) + 4 (size) + 4 (flags) + 4 (pad)


def pack_entry(name: str, offset: int, size: int) -> bytes:
    name_bytes = name.encode("utf-8")[:NAME_MAX - 1]
    name_field = name_bytes + b"\x00" * (NAME_MAX - len(name_bytes))
    return name_field + struct.pack("<III", offset, size, 0) + struct.pack("<I", 0)


def build_image(pairs, output_path):
    if not pairs:
        print("error: no files specified", file=sys.stderr)
        sys.exit(1)

    file_data = []
    for host_path, _ in pairs:
        with open(host_path, "rb") as f:
            file_data.append(f.read())

    n = len(pairs)
    header_size = 16
    dir_size    = n * ENTRY_SIZE
    data_start  = header_size + dir_size

    offsets = []
    cur = data_start
    for d in file_data:
        offsets.append(cur)
        cur += len(d)

    total = cur

    with open(output_path, "wb") as out:
        # Header
        out.write(struct.pack("<IIII", MAGIC, VERSION, n, 0))
        # Entries
        for i, (_, rd_name) in enumerate(pairs):
            out.write(pack_entry(rd_name, offsets[i], len(file_data[i])))
        # Data
        for d in file_data:
            out.write(d)

    print("[mkinitrd] wrote {}  ({} bytes, {} file(s))".format(output_path, total, n))
    for i, (hp, rn) in enumerate(pairs):
        print("           {:<40}  {} bytes  <- {}".format(rn, len(file_data[i]), hp))


def main():
    parser = argparse.ArgumentParser(description="AIOS initrd image builder")
    parser.add_argument("-o", "--output", required=True, help="Output image path")
    parser.add_argument(
        "files", nargs="*",
        help="Alternating host-path ramdisk-path pairs"
    )
    args = parser.parse_args()

    if len(args.files) % 2 != 0:
        print("error: files must be host/ramdisk pairs", file=sys.stderr)
        sys.exit(1)

    pairs = [(args.files[i], args.files[i + 1])
             for i in range(0, len(args.files), 2)]

    build_image(pairs, args.output)


if __name__ == "__main__":
    main()
