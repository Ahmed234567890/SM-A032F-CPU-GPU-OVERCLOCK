# Device-tree patches

These files are the authoritative reference for the overclock. They are plain
unified diffs against **decompiled** device trees, not against the vendor DTS
source. That matters:

- The numeric values are the values that are actually in the live FDT.
- `dtc` renumbers phandles when a tree is decompiled and recompiled, so a raw
  `patch -p1` is not reliable across firmware builds.
- Use the diffs to see **what changed**, then apply the change to your own tree
  with `dtc`/`fdtput` as described in `docs/BUILD-AND-FLASH.md`.

---

## Files

| File | Base | Target | Changes |
|---|---|---|---|
| `cpu-2ghz-1.6ghz.diff` | stock FDT | CPU v4 | 4 little tables + 4 big tables |
| `gpu-650mhz.diff` | CPU v4 FDT | CPU v4 + GPU 650 | GPU OPP, dvfs-list, assigned clock |
| `gpu-700mhz-preview.diff` | CPU v4 + GPU 650 | GPU 700 preview | 650 → 700 values only |

The CPU diff applies to `cpufreq-clus0` (little) and `cpufreq-clus1` (big).
Only `operating-points`, `-1`, `-2`, and `-3` change. Every `-65` thermal table
is untouched.

The GPU diffs apply to `/soc/mm/gpu@60000000` inside the `mm` bus node.

---

## CPU values

Little cluster, prepended to each of the four tables:

```text
0x186a00 = 1,600,000 kHz
0x1312d0 = 1,250,000 uV
```

Big cluster, prepended to each of the four tables:

```text
0x1e8480 = 2,000,000 kHz
0x1312d0 = 1,250,000 uV
```

The stock little table tops out at 1.2 GHz and the stock big table tops out at
1.6 GHz, so both entries are new OPPs, not duplicates.

The efuse bin on the reference phone is `cpu1_bin = 2`, so the big cluster reads
`operating-points-2`. Patching only `operating-points` has no effect. Patch all
four base tables to be independent of the bin.

---

## GPU values

| Property | Stock 550 | 650 MHz | 700 MHz preview |
|---|---:|---:|---:|
| `operating-points` third entry | `0x86470` | `0x9eb10` | `0xaae60` |
| `sprd,dvfs-lists` third entry | `0x86470 0 0x09 0x01` | `0x9eb10 0 0x09 0x01` | `0xaae60 0 0x09 0x01` |
| `assigned-clocks` | absent | `<0x06 0x17>` | `<0x06 0x17>` |
| `assigned-clock-rates` | absent | `650000000` | `700000000` |

`0x06` is the GPLL provider phandle in the reference tree, and `0x17` is
`CLK_GPLL`. The parent index `0x09` is unchanged; the GPU driver still selects
GPLL. The new part is that GPLL itself is programmed before the GPU driver
probes.

`0xaae60` = 700,000 kHz. The 700 file is a **preview only**; it is not enabled
by this project.

---

## Applying the patch

The recommended path is to decompile your own `fdt.dtb`, edit the two CPU nodes
and/or the GPU node, recompile, and pack:

```sh
python3 scripts/unpack_boot.py --image boot-stock.img --outdir work
dtc -I dtb -O dts -o work/fdt.dts work/fdt.dtb
# edit work/fdt.dts using the values above
dtc -I dts -O dtb -o work/fdt-patched.dtb work/fdt.dts
python3 scripts/pack_dtbo.py --image boot-stock.img \
  --fdt work/fdt-patched.dtb --out boot-oc.img
python3 scripts/verify_boot.py --image boot-oc.img --reference boot-stock.img
```

If you prefer to keep the binary tree byte-identical outside the edited
properties, use `fdtput -t x`; see the GPU document for the exact commands.
`fdtput` replaces the whole property, so include every value.

---

## Provenance and license

The base device tree is the vendor tree shipped in the SM-A032F boot image and
is licensed **GPL-2.0**. These diffs and the values they contain are derived
from that tree and remain GPL-2.0. The scripts and documentation in this
repository are MIT; see the top-level `LICENSE`.

No boot images, kernels, vendor blobs, or GPU firmware are included. The
reference hashes in `boot-image-layout.md` are for verification only.
