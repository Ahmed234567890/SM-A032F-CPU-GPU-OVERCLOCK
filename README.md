# SM-A032F CPU + GPU overclock

**Samsung Galaxy A03 Core (SM-A032F) · Unisoc SC9863A · PowerVR Rogue GE8322**

A reproducible, evidence-backed guide to running the two Cortex-A55 clusters at
**2.0 GHz big / 1.6 GHz little** and the PowerVR GPU at **650 MHz** on the stock
vendor kernel. The kernel, ramdisk, and vendor GPU firmware are not modified:
everything is done by editing the device tree that is embedded in the boot image.

> **Status:** CPU configuration validated. GPU 650 MHz is flashed and has passed
> three 60-second stress runs with zero HWR/CRR/SLR/WGP/TRP/FWF errors. The
> 700 MHz GPU step is documented as a preview but is **not** enabled.

---

## Results at a glance

| Domain | Stock | This guide | Measured result | Status |
|---|---|---|---|---|
| CPU big cluster (cores 4–7) | 1.6 GHz | **2.0 GHz @ 1.25 V** | 49.22 vs 39.66 Miter/s single-core | validated |
| CPU little cluster (cores 0–3) | 1.2 GHz @ ~1.10 V | **1.6 GHz @ 1.25 V** | 340.78 vs 277.17 Miter/s all-core | validated |
| GPU (PowerVR GE8322) | 550 MHz | **650 MHz** | 7.97 vs 7.31 fps (20 s A/B); 8.15–8.47 fps over 3 × 60 s | 3 × 60 s clean |
| DDR | 933 MHz | 933 MHz (no change) | 6.66 GB/s write, 5.74 GB/s read | measured |

The CPU overclock buys about **23 % more all-core throughput** for roughly
**1.67× the estimated dynamic power**. The GPU gain depends on the workload:
the best 60 s run was **+15.9 %**, and a same-length 20 s A/B run was **+9.0 %**.
The theoretical clock ratio is +18.2 %; the gap is explained in
[docs/GPU-OVERCLOCK.md](docs/GPU-OVERCLOCK.md#9-measured-result).

---

## The tricks

Most guides stop at "add a frequency to `operating-points`". On this device that
does nothing. There are three non-obvious mechanisms behind this port.

### 1. The CPU reads a bin table, not the base table

The SC9863A efuse reports a DVFS bin per cluster. On this phone the big cluster
reports `cpu1_bin = 2`, so the vendor cpufreq driver appends `-2` and reads
`operating-points-2`. Patching only `operating-points` is silently ignored.

The patch therefore prepends the new OPP to **all four** tables:

```text
operating-points
operating-points-1
operating-points-2   <-- the one this phone actually uses
operating-points-3
```

The `-65` thermal tables are deliberately left stock. At 65 °C the little
cluster falls back to 1.2 GHz and the big cluster to 1.6 GHz; this is a safety
feature, not a bug.

### 2. The GPU driver changes parents, not PLL rates

The vendor `pvrsrvkm` driver imports `clk_set_parent` but **not**
`clk_set_rate`. Changing the GPU OPP from 550 to 650 MHz only makes the driver
select the same `gpll` parent again; GPLL keeps its old rate.

The fix is a pair of properties on the GPU node:

```dts
assigned-clocks = <&gpll CLK_GPLL>;
assigned-clock-rates = <650000000>;
```

`platform_drv_probe()` calls `of_clk_set_defaults()` **before** `pvrsrvkm` probes,
so the kernel programs GPLL to 650 MHz first. The GPU driver then reparents
`gpu-core`/`gpu-soc` to a PLL that is already running at the new rate. GPLL feeds
only the GPU on this SoC, so no other peripheral is affected.

### 3. The DTBO header is big-endian

The bootloader expects the Android DTBO container magic as the bytes
`d7 b7 ab 1e`. A little-endian write produces `1e ab b7 d7`, and the bootloader
silently ignores the appended tree. An early 2.1 GHz image was lost to this.
The packers in `scripts/` always write big-endian.

---

## How it fits together

```mermaid
flowchart LR
  A[boot.img] --> B[Android boot header<br/>kernel + ramdisk]
  A --> C[DTBO container<br/>at 0x13be000]
  C --> D[FDT / device tree]
  D --> E[cpufreq-clus0<br/>little OPP tables]
  D --> F[cpufreq-clus1<br/>big OPP tables]
  D --> G[gpu@60000000<br/>OPP + assigned-clock-rates]
  E --> H[CPU DVFS]
  F --> H
  G --> I[GPLL 650 MHz]
  I --> J[gpu-core / gpu-soc]
```

Only the boot partition is touched. `pm_sys`, `sml`, `trustos`, `uboot`, and the
`dtbo` partition are not modified.

---

## Repository map

| Path | What it contains |
|---|---|
| [docs/CPU-OVERCLOCK.md](docs/CPU-OVERCLOCK.md) | DVFS bins, exact OPP patch, voltage/thermal analysis, tuning |
| [docs/GPU-OVERCLOCK.md](docs/GPU-OVERCLOCK.md) | GPLL topology, driver evidence, PLL math, 650/700 trade-offs |
| [docs/BUILD-AND-FLASH.md](docs/BUILD-AND-FLASH.md) | boot-image layout, DTBO packing, `dd` flash, verification, rollback |
| [docs/TESTING.md](docs/TESTING.md) | CPU/GPU/RAM benchmarks, expected numbers, abort criteria |
| [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | every failure mode found while building this |
| [patches/](patches/) | focused DTS diffs, GPLL register table, boot layout |
| [scripts/](scripts/) | pack/unpack/verify tools, PLL calculator, stress sources |
| [results/](results/) | sanitized benchmark results and raw CPU logs |

---

## What you need

| Item | Requirement |
|---|---|
| Device | Samsung SM-A032F (Galaxy A03 Core), Unisoc SC9863A |
| Firmware | Vendor kernel `4.14.199`, build `A032FXXS9CZA2` or a compatible boot image |
| Bootloader | Unlocked (`ro.boot.flash.locked=0`; orange verified-boot state) |
| Root | Required to write the boot partition and to read GPU/CPU sysfs |
| Shell | Termux or `adb` shell with `su`; a Linux host for the Python tools |
| Host tools | Python 3, `dtc` (device-tree compiler), optional `fdtput`/`fdtdump` |
| Recovery | Stock boot image and a working download-mode/Odin fallback |
| Battery | At least 50 %, device cool before flashing |

The phone's userspace is 32-bit (`armeabi-v7a`), so the benchmark binaries in
this repo are built as ARMv7. The boot image itself is the vendor 64-bit kernel.

---

## Quick start

The full step-by-step is in [docs/BUILD-AND-FLASH.md](docs/BUILD-AND-FLASH.md).
The short version:

```sh
# 0. Read DISCLAIMER.md. Back up the stock boot partition first.
export PHONE="<phone-ip>"
export SSH_PORT="<ssh-port>"
export SSH_USER="<termux-user>"
export SSH_KEY="$HOME/.ssh/id_ed25519"
export BOOT="/dev/block/by-name/boot"

# 1. Dump the live boot partition and copy it off the phone.
ssh -p "$SSH_PORT" -i "$SSH_KEY" "$SSH_USER@$PHONE" \
  "su -c 'dd if=$BOOT of=/sdcard/boot-stock.img bs=1M'"
scp -P "$SSH_PORT" -i "$SSH_KEY" \
  "$SSH_USER@$PHONE:/sdcard/boot-stock.img" ./boot-stock.img

# 2. Unpack, patch, and repack (see docs for the DTS edit).
python3 scripts/unpack_boot.py --image boot-stock.img --outdir work
# ... edit work/fdt.dts, then:
dtc -I dts -O dtb -o work/fdt-patched.dtb work/fdt.dts
python3 scripts/pack_dtbo.py \
  --image boot-stock.img --fdt work/fdt-patched.dtb --out boot-oc.img

# 3. Verify before flashing.
python3 scripts/verify_boot.py --image boot-oc.img

# 4. Flash only the boot partition, then reboot.
scp -P "$SSH_PORT" -i "$SSH_KEY" boot-oc.img "$SSH_USER@$PHONE:/sdcard/"
ssh -p "$SSH_PORT" -i "$SSH_KEY" "$SSH_USER@$PHONE" \
  "su -c 'dd if=/sdcard/boot-oc.img of=$BOOT bs=1M conv=fsync; sync; reboot'"
```

After reboot:

```sh
su -c 'cat /sys/devices/system/cpu/cpufreq/policy4/cpuinfo_max_freq'
# expected: 2000000

su -c 'cat /sys/class/devfreq/60000000.gpu/max_freq'
# expected: 650000000
```

---

## Validation status

| Test | Result |
|---|---|
| CPU 2.0 GHz OPP present and active | pass (`policy4/cpuinfo_max_freq=2000000`, 100 % residency in 3 s burst) |
| CPU 1.6 GHz little OPP at 1.25 V | pass (`vddcpu=1250000` during stress) |
| CPU thermal fallback | pass (65 °C table switches to 1.2/1.6 GHz, no panic) |
| CPU all-core 3 s benchmark | pass (340.78 Miter/s; repeat 336.18) |
| GPU 650 MHz OPP present | pass (`max_freq=650000000`, `available_frequencies=256/384/650`) |
| GPU 650 MHz 60 s stress × 3 | pass (509 / 489 / 502 frames; zero error counters) |
| GPU thermal | pass (GPU ≤ 59.9 °C, CPU ≤ 65.9 °C) |
| GPU 700 MHz | **not enabled**; preview only, needs a voltage/timing plan |
| DDR 1000 MHz | **not attempted**; 933 MHz is the top PM-firmware OPP |

A strict 60 s **stock** GPU run is still on the to-do list. The stock baseline in
this repo is a 20 s run (147 frames, 7.31 fps); the 650 MHz 20 s A/B run was
160 frames / 7.97 fps. The 60 s 650 MHz numbers are reported separately because
the tool prints a cumulative average and the first seconds include shader/ramp
overhead.

---

## Safety

This is a real overclock of a real phone. Read [DISCLAIMER.md](DISCLAIMER.md)
before you start. In short:

- Back up the stock boot image and keep a second copy off the phone.
- Change only the `boot` partition. Do not flash `pm_sys`, `sml`, `trustos`, or
  `uboot` as part of this guide.
- Stop at the first GPU error counter, visual corruption, boot loop, or
  temperature above 70 °C.
- 700 MHz at the fixed 900 mV `vddcore` has less timing margin. Do not enable it
  just because 650 MHz passed.
- Overclocking can void the warranty, reduce silicon lifetime, and corrupt data.
  You are responsible for your device.

---

## License

Original code and documentation: MIT. The device-tree patches are derived from
the vendor GPL-2.0 device tree and remain GPL-2.0. See [LICENSE](LICENSE) and
[patches/README.md](patches/README.md).

This project is not affiliated with Samsung, Unisoc, or Imagination Technologies.
