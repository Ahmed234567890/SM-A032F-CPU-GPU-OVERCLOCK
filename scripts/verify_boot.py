#!/usr/bin/env python3
"""Verify an SM-A032F boot image before flashing.

Checks the boot header, payload offsets, DTBO container (including byte order),
FDT header, and optional GPU/CPU properties. With --reference, also verifies
that the kernel, ramdisk, and boot-image id are unchanged.

Example:
    python3 verify_boot.py --image boot-oc.img --reference boot-stock.img
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

DTBO_MAGIC = 0xD7B7AB1E
FDT_MAGIC = b"\xd0\x0d\xfe\xed"

FDT_BEGIN_NODE = 1
FDT_END_NODE = 2
FDT_PROP = 3
FDT_NOP = 4
FDT_END = 9


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def align_up(value: int, page: int) -> int:
    return (value + page - 1) // page * page


def parse_int(text: str) -> int:
    return int(text, 0)


def u32be(data: bytes, off: int) -> int:
    return struct.unpack_from(">I", data, off)[0]


def read_cstr(data: bytes, off: int) -> str:
    end = data.index(b"\0", off)
    return data[off:end].decode("ascii", "replace")


def fdt_header(fdt: bytes) -> dict:
    if len(fdt) < 40 or fdt[:4] != FDT_MAGIC:
        raise ValueError("bad FDT magic")
    fields = struct.unpack_from(">10I", fdt, 0)
    keys = [
        "magic", "totalsize", "off_dt_struct", "off_dt_strings",
        "off_mem_rsvmap", "version", "last_comp_version",
        "boot_cpuid_phys", "size_dt_strings", "size_dt_struct",
    ]
    return dict(zip(keys, fields))


def fdt_node_props(fdt: bytes, target: str) -> dict[str, bytes]:
    """Return all properties of one node, using the FDT struct block."""
    h = fdt_header(fdt)
    pos = h["off_dt_struct"]
    strings = h["off_dt_strings"]
    stack: list[str] = []
    current: str | None = None
    props: dict[str, bytes] = {}
    end = min(len(fdt), h["off_dt_struct"] + h["size_dt_struct"])

    while pos + 4 <= end:
        token = u32be(fdt, pos)
        pos += 4
        if token == FDT_BEGIN_NODE:
            name = read_cstr(fdt, pos)
            pos += len(name.encode("ascii", "replace")) + 1
            pos = align_up(pos, 4)
            if name.startswith("/"):
                path = name
            elif not stack:
                path = "/" + name if name else "/"
            else:
                parent = stack[-1].rstrip("/")
                path = parent + "/" + name
            stack.append(path)
            current = path
        elif token == FDT_END_NODE:
            if stack:
                stack.pop()
            current = stack[-1] if stack else None
        elif token == FDT_PROP:
            length = u32be(fdt, pos)
            nameoff = u32be(fdt, pos + 4)
            pos += 8
            value = fdt[pos:pos + length]
            pos = align_up(pos + length, 4)
            if current == target:
                name = read_cstr(fdt, strings + nameoff)
                props[name] = value
        elif token == FDT_NOP:
            continue
        elif token == FDT_END:
            break
        else:
            raise ValueError(f"unexpected FDT token {token} at 0x{pos - 4:x}")
    return props


def cells(value: bytes) -> list[int]:
    return [u32be(value, i) for i in range(0, len(value) - 3, 4)]


def fmt_cells(values: list[int]) -> str:
    return " ".join(f"{v:x}" for v in values) if values else "(empty)"


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Verify an SM-A032F boot image.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("--image", required=True, type=Path)
    ap.add_argument("--reference", type=Path, default=None,
                    help="known-good source image for kernel/ramdisk/id comparison")
    ap.add_argument("--expect-sha256", default=None)
    ap.add_argument("--dtbo-off", type=parse_int, default=None)
    ap.add_argument("--dtb-size-off", type=parse_int, default=0x670)
    ap.add_argument("--no-props", action="store_true",
                    help="skip the FDT property checks")
    args = ap.parse_args()

    image = args.image.read_bytes()
    failures: list[str] = []
    warnings: list[str] = []
    out: list[str] = []

    def line(text: str) -> None:
        out.append(text)

    line(f"image            : {args.image}")
    line(f"image_size       : {len(image)} (0x{len(image):x})")
    line(f"image_sha256     : {sha256(image)}")

    if args.expect_sha256 and sha256(image).lower() != args.expect_sha256.lower():
        failures.append(f"image SHA-256 {sha256(image)} != {args.expect_sha256}")

    if len(image) < 0x678 or image[:8] != b"ANDROID!":
        print("\n".join(out))
        print("FAIL: not an Android boot image")
        return 1

    kernel_size = struct.unpack_from("<I", image, 0x08)[0]
    kernel_addr = struct.unpack_from("<I", image, 0x0C)[0]
    ramdisk_size = struct.unpack_from("<I", image, 0x10)[0]
    ramdisk_addr = struct.unpack_from("<I", image, 0x14)[0]
    second_size = struct.unpack_from("<I", image, 0x18)[0]
    page_size = struct.unpack_from("<I", image, 0x24)[0]
    header_version = struct.unpack_from("<I", image, 0x28)[0]
    boot_id = image[0x240:0x260]
    dtb_size = struct.unpack_from("<I", image, args.dtb_size_off)[0]
    dtb_addr = struct.unpack_from("<I", image, args.dtb_size_off + 4)[0]

    line(f"header_version   : {header_version}")
    line(f"page_size        : 0x{page_size:x}")
    line(f"kernel_size      : {kernel_size} (0x{kernel_size:x})")
    line(f"ramdisk_size     : {ramdisk_size} (0x{ramdisk_size:x})")
    line(f"dtb_size         : {dtb_size} (0x{dtb_size:x})")
    line(f"dtb_addr         : 0x{dtb_addr:x}")
    line(f"boot_id          : {boot_id.hex()}")

    if header_version < 2:
        warnings.append(f"header_version {header_version} has no dtb_size field")
    if page_size not in (0x800, 0x1000, 0x2000, 0x4000, 0x8000, 0x10000):
        failures.append(f"unexpected page size 0x{page_size:x}")

    kernel_off = page_size
    ramdisk_off = align_up(kernel_off + kernel_size, page_size)
    second_off = align_up(ramdisk_off + ramdisk_size, page_size)
    computed_dtbo_off = align_up(second_off + second_size, page_size)
    dtbo_off = args.dtbo_off if args.dtbo_off is not None else computed_dtbo_off

    kernel = image[kernel_off:kernel_off + kernel_size]
    ramdisk = image[ramdisk_off:ramdisk_off + ramdisk_size]
    line(f"kernel_off       : 0x{kernel_off:x}")
    line(f"kernel_sha256    : {sha256(kernel)}")
    line(f"ramdisk_off      : 0x{ramdisk_off:x}")
    line(f"ramdisk_sha256   : {sha256(ramdisk)}")
    line(f"dtbo_off         : 0x{dtbo_off:x}")

    if len(kernel) != kernel_size:
        failures.append("kernel payload is truncated")
    if len(ramdisk) != ramdisk_size:
        failures.append("ramdisk payload is truncated")

    if dtb_size == 0:
        failures.append("dtb_size is zero")
    elif dtbo_off + dtb_size > len(image):
        failures.append("DTBO container extends past the end of the image")
    else:
        dtbo = image[dtbo_off:dtbo_off + dtb_size]
        magic_be = struct.unpack_from(">I", dtbo, 0)[0]
        magic_le = struct.unpack_from("<I", dtbo, 0)[0]
        if magic_be == DTBO_MAGIC:
            line("dtbo_magic       : d7b7ab1e (big-endian, OK)")
            endian = ">"
        elif magic_le == DTBO_MAGIC:
            line("dtbo_magic       : d7b7ab1e (little-endian, BYTE-SWAPPED)")
            failures.append("DTBO header is byte-swapped; the bootloader will reject it")
            endian = "<"
        else:
            line(f"dtbo_magic       : be=0x{magic_be:08x} le=0x{magic_le:08x} (BAD)")
            failures.append("no DTBO magic at the computed offset")
            endian = ">"

        if magic_be == DTBO_MAGIC or magic_le == DTBO_MAGIC:
            total_size, header_size, entry_size, entry_count, \
                entries_offset, dtbo_page, version = struct.unpack_from(
                    endian + "7I", dtbo, 4)
            line(f"dtbo_total_size  : {total_size} (0x{total_size:x})")
            line(f"dtbo_header_size : 0x{header_size:x}")
            line(f"dtbo_entry_size  : 0x{entry_size:x}")
            line(f"dtbo_entry_count : {entry_count}")
            line(f"dtbo_entries_off : 0x{entries_offset:x}")
            line(f"dtbo_page_size   : 0x{dtbo_page:x}")
            line(f"dtbo_version     : {version}")

            if total_size != dtb_size:
                failures.append(
                    f"dtbo total_size 0x{total_size:x} != dtb_size 0x{dtb_size:x}")
            if total_size > len(dtbo):
                failures.append("dtbo total_size exceeds the extracted container")
            if entry_count < 1:
                failures.append("DTBO has no entries")
            else:
                dt_size, dt_offset = struct.unpack_from(
                    endian + "2I", dtbo, entries_offset)
                line(f"fdt_size         : {dt_size} (0x{dt_size:x})")
                line(f"fdt_offset       : 0x{dt_offset:x}")
                fdt = dtbo[dt_offset:dt_offset + dt_size]
                if len(fdt) != dt_size or fdt[:4] != FDT_MAGIC:
                    failures.append("FDT payload magic/truncation")
                else:
                    h = fdt_header(fdt)
                    line(f"fdt_totalsize    : {h['totalsize']} "
                         f"(0x{h['totalsize']:x})")
                    line(f"fdt_version      : {h['version']}")
                    if h["totalsize"] != len(fdt):
                        failures.append(
                            f"FDT totalsize {h['totalsize']} != payload {len(fdt)}")

                    if not args.no_props:
                        try:
                            gpu = fdt_node_props(fdt, "/soc/mm/gpu@60000000")
                        except Exception as exc:
                            gpu = {}
                            warnings.append(f"FDT GPU property parse failed: {exc}")
                        if gpu:
                            op = cells(gpu.get("operating-points", b""))
                            dv = cells(gpu.get("sprd,dvfs-lists", b""))
                            ac = cells(gpu.get("assigned-clocks", b""))
                            ar = cells(gpu.get("assigned-clock-rates", b""))
                            line(f"gpu operating-points : {fmt_cells(op)}")
                            line(f"gpu dvfs-lists       : {fmt_cells(dv)}")
                            line(f"gpu assigned-clocks  : {fmt_cells(ac)}")
                            line(f"gpu assigned-rates   : {fmt_cells(ar)}")
                            if len(op) >= 2:
                                top_khz = op[-2]
                                line(f"gpu top OPP          : {top_khz} kHz "
                                     f"({top_khz / 1000:.0f} MHz)")
                                if top_khz not in (550000, 650000, 700000):
                                    warnings.append(
                                        f"unexpected GPU top OPP {top_khz} kHz")
                            if len(dv) >= 4:
                                top_dv = dv[-4]
                                if len(op) >= 2 and top_dv != op[-2]:
                                    failures.append(
                                        "sprd,dvfs-lists top frequency does not "
                                        "match operating-points")
                            if ac and (len(ac) < 2 or ac[-1] != 0x17):
                                warnings.append(
                                    "assigned-clocks does not end in CLK_GPLL (0x17)")
                            if ar and ar[-1] not in (550000000, 650000000, 700000000):
                                warnings.append(
                                    f"unexpected assigned-clock-rate {ar[-1]}")

                        for node in ("/cpufreq-clus0", "/cpufreq-clus1"):
                            try:
                                props = fdt_node_props(fdt, node)
                            except Exception:
                                props = {}
                            if not props:
                                continue
                            line(f"cpu node             : {node}")
                            for pname in ("operating-points", "operating-points-1",
                                          "operating-points-2", "operating-points-3"):
                                if pname in props:
                                    vals = cells(props[pname])
                                    if len(vals) >= 2:
                                        line(f"  {pname:20s}: top "
                                             f"{vals[0]} kHz / {vals[1]} uV")

    if args.reference:
        ref = args.reference.read_bytes()
        line(f"reference        : {args.reference}")
        line(f"reference_sha256 : {sha256(ref)}")
        if len(ref) < 0x678 or ref[:8] != b"ANDROID!":
            failures.append("reference is not an Android boot image")
        else:
            r_ks = struct.unpack_from("<I", ref, 0x08)[0]
            r_rs = struct.unpack_from("<I", ref, 0x10)[0]
            r_page = struct.unpack_from("<I", ref, 0x24)[0]
            r_ko = r_page
            r_ro = align_up(r_ko + r_ks, r_page)
            r_kernel = ref[r_ko:r_ko + r_ks]
            r_ramdisk = ref[r_ro:r_ro + r_rs]
            r_id = ref[0x240:0x260]
            if kernel != r_kernel:
                failures.append("kernel differs from the reference")
            else:
                line("kernel           : identical to reference")
            if ramdisk != r_ramdisk:
                failures.append("ramdisk differs from the reference")
            else:
                line("ramdisk          : identical to reference")
            if boot_id != r_id:
                failures.append("boot-image id differs from the reference")
            else:
                line("boot id          : identical to reference")

    print("\n".join(out))
    if warnings:
        print("\nWARNINGS")
        for w in warnings:
            print(f"  - {w}")
    if failures:
        print("\nFAIL")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("\nPASS: image structure and payload checks are consistent.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
