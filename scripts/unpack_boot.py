#!/usr/bin/env python3
"""Unpack an SM-A032F boot image into kernel, ramdisk, DTBO, and FDT.

Reads the Android boot header, computes the page-aligned payload offsets,
extracts the DTBO container and its first FDT, and writes a layout report.
Optionally decompiles the FDT to DTS with dtc.

Example:
    python3 unpack_boot.py --image boot-stock.img --outdir work
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import struct
import subprocess
import sys
from pathlib import Path

DTBO_MAGIC = 0xD7B7AB1E
FDT_MAGIC = b"\xd0\x0d\xfe\xed"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def align_up(value: int, page: int) -> int:
    return (value + page - 1) // page * page


def parse_int(text: str) -> int:
    return int(text, 0)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Unpack an SM-A032F boot image.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("--image", required=True, type=Path)
    ap.add_argument("--outdir", required=True, type=Path)
    ap.add_argument("--dtbo-off", type=parse_int, default=None,
                    help="override the computed DTBO container offset")
    ap.add_argument("--no-dtc", action="store_true",
                    help="do not run dtc to produce fdt.dts")
    args = ap.parse_args()

    image = args.image.read_bytes()
    if len(image) < 0x678:
        sys.exit("image is too small to contain a boot header")
    if image[:8] != b"ANDROID!":
        sys.exit("not an Android boot image (missing ANDROID! magic)")

    kernel_size = struct.unpack_from("<I", image, 0x08)[0]
    kernel_addr = struct.unpack_from("<I", image, 0x0C)[0]
    ramdisk_size = struct.unpack_from("<I", image, 0x10)[0]
    ramdisk_addr = struct.unpack_from("<I", image, 0x14)[0]
    second_size = struct.unpack_from("<I", image, 0x18)[0]
    second_addr = struct.unpack_from("<I", image, 0x1C)[0]
    tags_addr = struct.unpack_from("<I", image, 0x20)[0]
    page_size = struct.unpack_from("<I", image, 0x24)[0]
    header_version = struct.unpack_from("<I", image, 0x28)[0]
    os_version = struct.unpack_from("<I", image, 0x2C)[0]
    name = image[0x30:0x40].split(b"\0")[0].decode("ascii", "replace")
    cmdline = image[0x40:0x240].split(b"\0")[0].decode("ascii", "replace")
    boot_id = image[0x240:0x260]

    if page_size not in (0x800, 0x1000, 0x2000, 0x4000, 0x8000, 0x10000):
        sys.exit(f"unexpected page size 0x{page_size:x}")

    dtb_size = 0
    dtb_addr = 0
    if header_version >= 2 and len(image) >= 0x678:
        dtb_size = struct.unpack_from("<I", image, 0x670)[0]
        dtb_addr = struct.unpack_from("<I", image, 0x674)[0]

    kernel_off = page_size
    ramdisk_off = align_up(kernel_off + kernel_size, page_size)
    second_off = align_up(ramdisk_off + ramdisk_size, page_size)
    computed_dtbo_off = align_up(second_off + second_size, page_size)
    dtbo_off = args.dtbo_off if args.dtbo_off is not None else computed_dtbo_off

    kernel = image[kernel_off:kernel_off + kernel_size]
    ramdisk = image[ramdisk_off:ramdisk_off + ramdisk_size]

    if len(kernel) != kernel_size:
        sys.exit("kernel payload is truncated")
    if len(ramdisk) != ramdisk_size:
        sys.exit("ramdisk payload is truncated")

    args.outdir.mkdir(parents=True, exist_ok=True)
    (args.outdir / "kernel.bin").write_bytes(kernel)
    (args.outdir / "ramdisk.bin").write_bytes(ramdisk)

    lines = [
        f"image            : {args.image}",
        f"image_size       : {len(image)} (0x{len(image):x})",
        f"image_sha256     : {sha256(image)}",
        f"header_version   : {header_version}",
        f"os_version       : 0x{os_version:08x}",
        f"page_size        : 0x{page_size:x}",
        f"name             : {name}",
        f"cmdline          : {cmdline}",
        f"boot_id          : {boot_id.hex()}",
        f"kernel_size      : {kernel_size} (0x{kernel_size:x})",
        f"kernel_addr      : 0x{kernel_addr:x}",
        f"kernel_off       : 0x{kernel_off:x}",
        f"kernel_sha256    : {sha256(kernel)}",
        f"ramdisk_size     : {ramdisk_size} (0x{ramdisk_size:x})",
        f"ramdisk_addr     : 0x{ramdisk_addr:x}",
        f"ramdisk_off      : 0x{ramdisk_off:x}",
        f"ramdisk_sha256   : {sha256(ramdisk)}",
        f"second_size      : {second_size}",
        f"second_addr      : 0x{second_addr:x}",
        f"second_off       : 0x{second_off:x}",
        f"tags_addr        : 0x{tags_addr:x}",
        f"dtb_size         : {dtb_size} (0x{dtb_size:x})",
        f"dtb_addr         : 0x{dtb_addr:x}",
        f"computed_dtbo_off: 0x{computed_dtbo_off:x}",
        f"dtbo_off         : 0x{dtbo_off:x}",
    ]

    if dtb_size == 0:
        lines.append("dtbo             : absent (dtb_size is zero)")
        (args.outdir / "layout.txt").write_text("\n".join(lines) + "\n")
        print("\n".join(lines))
        return 0

    if dtbo_off + dtb_size > len(image):
        sys.exit("DTBO container extends past the end of the image")

    dtbo = image[dtbo_off:dtbo_off + dtb_size]
    (args.outdir / "dtbo.img").write_bytes(dtbo)

    magic_be = struct.unpack_from(">I", dtbo, 0)[0]
    magic_le = struct.unpack_from("<I", dtbo, 0)[0]
    if magic_be == DTBO_MAGIC:
        endian = ">"
        endian_name = "big-endian"
    elif magic_le == DTBO_MAGIC:
        endian = "<"
        endian_name = "little-endian (byte-swapped; bootloader will reject)"
    else:
        sys.exit(f"DTBO magic not found: be=0x{magic_be:08x} le=0x{magic_le:08x}")

    total_size, header_size, entry_size, entry_count, entries_offset, \
        dtbo_page_size, version = struct.unpack_from(endian + "7I", dtbo, 4)

    lines += [
        f"dtbo_magic       : 0x{DTBO_MAGIC:08x} ({endian_name})",
        f"dtbo_total_size  : {total_size} (0x{total_size:x})",
        f"dtbo_header_size : 0x{header_size:x}",
        f"dtbo_entry_size  : 0x{entry_size:x}",
        f"dtbo_entry_count : {entry_count}",
        f"dtbo_entries_off : 0x{entries_offset:x}",
        f"dtbo_page_size   : 0x{dtbo_page_size:x}",
        f"dtbo_version     : {version}",
    ]

    if entry_count < 1:
        sys.exit("DTBO container has no entries")
    entry_off = entries_offset
    dt_size, dt_offset = struct.unpack_from(endian + "2I", dtbo, entry_off)
    lines += [
        f"fdt_size         : {dt_size} (0x{dt_size:x})",
        f"fdt_offset       : 0x{dt_offset:x}",
    ]

    fdt = dtbo[dt_offset:dt_offset + dt_size]
    if len(fdt) != dt_size:
        sys.exit("FDT payload is truncated")
    if fdt[:4] != FDT_MAGIC:
        sys.exit("FDT magic missing in the DTBO payload")
    fdt_total = int.from_bytes(fdt[4:8], "big")
    if fdt_total != len(fdt):
        sys.exit(f"FDT totalsize {fdt_total} does not match payload length {len(fdt)}")

    (args.outdir / "fdt.dtb").write_bytes(fdt)
    lines += [
        f"fdt_totalsize    : {fdt_total} (0x{fdt_total:x})",
        f"fdt_sha256       : {sha256(fdt)}",
    ]

    if not args.no_dtc and shutil.which("dtc"):
        dts_path = args.outdir / "fdt.dts"
        proc = subprocess.run(
            ["dtc", "-I", "dtb", "-O", "dts", "-o", str(dts_path),
             str(args.outdir / "fdt.dtb")],
            capture_output=True,
            text=True,
        )
        if proc.returncode == 0:
            lines.append(f"fdt_dts          : {dts_path}")
        else:
            lines.append("fdt_dts          : dtc failed (see stderr below)")
            lines.append(proc.stderr.strip()[:2000])
    elif not args.no_dtc:
        lines.append("fdt_dts          : dtc not installed; skipped")

    (args.outdir / "layout.txt").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
