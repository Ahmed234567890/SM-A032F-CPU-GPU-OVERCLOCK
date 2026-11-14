# Boot image and DTBO layout (SM-A032F reference)

This is the layout measured from the A032FXXS9CZA2 boot image used in this
project. **It is not universal.** Different firmware can move the DTBO, change
the page size, or omit the `dtb_size` field. Always run
`scripts/unpack_boot.py` against your own dump first.

---

## 1. Android boot header

| Offset | Size | Field | Reference value |
|---:|---:|---|---|
| `0x000` | 8 | magic | `ANDROID!` |
| `0x008` | 4 | `kernel_size` | `0x12f6810` (19,886,096) |
| `0x00c` | 4 | `kernel_addr` | `0x8000` |
| `0x010` | 4 | `ramdisk_size` | `0xc6319` (811,801) |
| `0x014` | 4 | `ramdisk_addr` | `0x1000000` |
| `0x018` | 4 | `second_size` | `0` |
| `0x01c` | 4 | `second_addr` | `0` |
| `0x020` | 4 | `tags_addr` | `0x100` |
| `0x024` | 4 | `page_size` | `0x800` |
| `0x028` | 4 | `header_version` | `2` |
| `0x02c` | 4 | `os_version` | `0x1600019c` |
| `0x030` | 16 | `name` | `SRPUH09A009` |
| `0x040` | 512 | `cmdline` | `console=ttyS1,115200n8` |
| `0x240` | 32 | `id` | vendor digest, preserved by the packer |
| `0x260` | 1024 | `extra_cmdline` | empty |
| `0x670` | 4 | `dtb_size` | container size (LE u32) |
| `0x674` | 4 | `dtb_addr` | `0x01f00000` |

The header is little-endian. The DTBO container that follows is big-endian.
That mixed endianness is normal for this platform and is a common source of
confusion.

---

## 2. Payload offsets

```text
kernel_off  = page_size
ramdisk_off = align_up(kernel_off + kernel_size, page_size)
second_off  = align_up(ramdisk_off + ramdisk_size, page_size)
dtbo_off    = align_up(second_off + second_size, page_size)
```

For the reference image:

```text
kernel_off  = 0x0000800
ramdisk_off = 0x12f7800
second_off  = 0x13be000
dtbo_off    = 0x13be000
```

`dtb_size` is the size of the **whole DTBO container** at `dtbo_off`, including
the 0x20-byte header and the 0x20-byte entry. It is not the FDT size.

---

## 3. DTBO container

| Offset | Size | Field | Reference value |
|---:|---:|---|---|
| `0x00` | 4 | magic (BE) | `0xd7b7ab1e` |
| `0x04` | 4 | `total_size` | `0x40 + fdt_size` |
| `0x08` | 4 | `header_size` | `0x20` |
| `0x0c` | 4 | `entry_size` | `0x20` |
| `0x10` | 4 | `entry_count` | `1` |
| `0x14` | 4 | `entries_offset` | `0x20` |
| `0x18` | 4 | `page_size` | `0x800` |
| `0x1c` | 4 | `version` | `0` |
| `0x20` | 4 | entry `dt_size` | FDT size |
| `0x24` | 4 | entry `dt_offset` | `0x40` |
| `0x28` | 4 | entry `id` | `0` |
| `0x2c` | 4 | entry `rev` | `0` |
| `0x30` | 16 | entry `custom[4]` | `0` |
| `0x40` | — | FDT payload | — |

Byte order check:

```text
valid:         d7 b7 ab 1e
byte-swapped:  1e ab b7 d7   <-- bootloader ignores the tree
```

The packer writes the magic with `struct.pack(">I", 0xD7B7AB1E)`. An early
2.1 GHz image in this project used the byte-swapped form and was probably never
parsed by the bootloader.

---

## 4. Reference hashes

All images are exactly 64 MiB (`0x4000000`). These hashes are **not** boot
images and are not downloadable from this repository; they identify the builds
used for the published measurements.

| Build | SHA-256 | DTBO `dtb_size` | FDT size | FDT SHA-256 |
|---|---|---:|---:|---|
| stock | `88597b9974260134f1c0532e9e7dcd9feaa7756b2cd9639e3b09bce1afacdbca` | `0x15e46` | 89,606 | `65a7102914d8ce6227e5d141f8ed6c524656c63650caef0a73a2a4671418c805` |
| CPU v4 | `94a650d5e4f258734a46888da5dcb4a288ef1e28d552af91a19a925eda2a7d62` | `0x15e86` | 89,670 | `95a11a7d40fc3ad734b60495bee33d38d8d8c43029df973aabe559001f0b0faa` |
| CPU v4 + GPU 650 | `b331d013c2d1910717f72767ca3fd494b73ca0f0ae8094168ccb43a465a52e2d` | `0x15ecf` | 89,743 | `6c896c7b4355e151a49314990fe2b602eb0d181d2b355ff1eecc22d391eef1e4` |

Kernel and ramdisk are byte-identical in all three images:

```text
kernel  SHA-256 d8962f0fdceb4310a3c7fce0bc15146f47c97da81d9bda1d167c8cb2e19bc4d4
ramdisk SHA-256 c0a592f37c95b23aee02f30ab098fdd4147f4c0b6b8a52143ca85fa0558e0bd2
boot id        78a37c7ba388cf0f81bdeddfbb9978165740a48a000000000000000000000000
```

The CPU v4 image was used as the reference for the GPU build. The reproduction
check is:

```sh
python3 scripts/pack_dtbo.py \
  --image boot-cpu-v4.img \
  --fdt fdt-gpu650.dtb \
  --out boot-cpu-v4-gpu650.img
# expected SHA-256:
# b331d013c2d1910717f72767ca3fd494b73ca0f0ae8094168ccb43a465a52e2d
```

If your source image or FDT differs, the hash will differ. That is expected;
compare the structure with `verify_boot.py`, not the hash.

---

## 5. Inspection commands

```sh
# Header fields and payload offsets.
python3 scripts/unpack_boot.py --image boot-stock.img --outdir work

# First DTBO bytes.
xxd -s 0x13be000 -l 32 boot-stock.img

# Full structural verification.
python3 scripts/verify_boot.py --image boot-oc.img --reference boot-stock.img

# FDT properties.
fdtget -t x work/fdt.dtb /soc/mm/gpu@60000000 operating-points
fdtget -t x work/fdt.dtb /soc/mm/gpu@60000000 assigned-clock-rates
fdtget -t x work/fdt.dtb /cpufreq-clus1 operating-points-2
```

Do not flash an image whose `dtb_size`, DTBO magic, or kernel/ramdisk hashes do
not match your own source.
