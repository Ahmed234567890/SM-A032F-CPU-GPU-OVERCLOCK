#!/usr/bin/env python3
"""Pack a patched FDT into an SM-A032F boot image.

Builds a standard Android DTBO container around a single FDT, inserts it at the
known DTBO offset, clears the old container tail, and updates the boot header
dtb_size field. The boot image id field and the kernel/ramdisk payloads are
left untouched.

The DTBO header is written big-endian (magic d7 b7 ab 1e). A little-endian
header is silently ignored by the bootloader on this device.

Example:
    python3 pack_dtbo.py --image boot-stock.img \
        --fdt fdt-patched.dtb --out boot-oc.img

The defaults match the SM-A032F A032FXXS9CZA2 boot layout. Verify the offsets
against your own image before flashing; override them if your layout differs.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

DTBO_MAGIC = 0xD7B7AB1E
FDT_MAGIC = b"\xd0\x0d\xfe\xed"
CONTAINER_HDR = 0x40  # DTBO header (0x20) + one entry (0x20)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def parse_int(text: str) -> int:
    return int(text, 0)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Pack a patched FDT into an SM-A032F boot image.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("--image", required=True, type=Path, help="source boot image")
    ap.add_argument("--fdt", required=True, type=Path, help="patched FDT (.dtb)")
    ap.add_argument("--out", required=True, type=Path, help="output boot image")
    ap.add_argument("--dtbo-off", type=parse_int, default=0x13BE000,
                    help="DTBO container offset in the boot image")
    ap.add_argument("--dtb-size-off", type=parse_int, default=0x670,
                    help="boot header dtb_size field offset")
    ap.add_argument("--clear-size", type=parse_int, default=0x16000,
                    help="bytes to clear at the DTBO offset before writing")
    ap.add_argument("--page-size", type=parse_int, default=0x800,
                    help="DTBO page size")
    ap.add_argument("--expect-source-sha256", default=None,
                    help="refuse to run unless the source image has this SHA-256")
    ap.add_argument("--allow-little-endian-source", action="store_true",
                    help="accept a byte-swapped source DTBO (diagnostics only)")
    args = ap.parse_args()

    image = bytearray(args.image.read_bytes())
    fdt = args.fdt.read_bytes()

    if args.expect_source_sha256:
        actual = sha256(bytes(image))
        if actual.lower() != args.expect_source_sha256.lower():
            sys.exit(f"source SHA-256 mismatch: {actual}")

    if len(fdt) < 8 or fdt[:4] != FDT_MAGIC:
        sys.exit("FDT magic missing (expected d0 0d fe ed)")
    fdt_total = int.from_bytes(fdt[4:8], "big")
    if fdt_total != len(fdt):
        sys.exit(f"FDT totalsize {fdt_total} does not match file length {len(fdt)}")

    if len(image) < args.dtbo_off + 8:
        sys.exit("source image is too small for the requested DTBO offset")

    src_magic_be = struct.unpack_from(">I", image, args.dtbo_off)[0]
    src_magic_le = struct.unpack_from("<I", image, args.dtbo_off)[0]
    if src_magic_be != DTBO_MAGIC:
        if src_magic_le == DTBO_MAGIC and args.allow_little_endian_source:
            print("warning: source DTBO is byte-swapped; replacing it", file=sys.stderr)
        else:
            sys.exit(
                f"no big-endian DTBO container at 0x{args.dtbo_off:x} "
                f"(found 0x{src_magic_be:08x})"
            )

    old_size = struct.unpack_from("<I", image, args.dtb_size_off)[0]
    if old_size > args.clear_size:
        sys.exit(
            f"old dtb_size 0x{old_size:x} is larger than --clear-size "
            f"0x{args.clear_size:x}; increase --clear-size or verify the offset"
        )

    total = CONTAINER_HDR + len(fdt)
    if total > args.clear_size:
        sys.exit(
            f"new container 0x{total:x} does not fit in --clear-size "
            f"0x{args.clear_size:x}"
        )
    if args.dtbo_off + args.clear_size > len(image):
        sys.exit("cleared region extends past the end of the image")

    header = struct.pack(
        ">8I",
        DTBO_MAGIC,
        total,
        0x20,          # header_size
        0x20,          # entry_size
        1,             # entry_count
        0x20,          # entries_offset
        args.page_size,
        0,             # version
    )
    entry = struct.pack(
        ">8I",
        len(fdt),      # dt_size
        0x40,          # dt_offset
        0, 0, 0, 0, 0, 0,
    )
    container = header + entry + fdt
    assert len(container) == total

    # Preserve the boot image id before clearing anything.
    boot_id = bytes(image[0x240:0x260])

    end = args.dtbo_off + args.clear_size
    image[args.dtbo_off:end] = b"\0" * args.clear_size
    image[args.dtbo_off:args.dtbo_off + total] = container
    struct.pack_into("<I", image, args.dtb_size_off, total)

    if bytes(image[0x240:0x260]) != boot_id:
        sys.exit("internal error: boot image id changed")

    args.out.write_bytes(image)

    print(f"source     : {args.image}  sha256={sha256(args.image.read_bytes())}")
    print(f"fdt        : {args.fdt}  size={len(fdt)}  sha256={sha256(fdt)}")
    print(f"container  : total=0x{total:x}  fdt=0x{len(fdt):x}  "
          f"dtb_size=0x{total:x}")
    print(f"dtbo off   : 0x{args.dtbo_off:x}  clear=0x{args.clear_size:x}  "
          f"page=0x{args.page_size:x}")
    print(f"boot id    : {boot_id.hex()} (unchanged)")
    print(f"image      : {args.out}  size={len(image)}  "
          f"sha256={sha256(bytes(image))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
