# CPU overclock — 2.0 GHz big / 1.6 GHz little

- **Device:** Samsung SM-A032F (Galaxy A03 Core)
- **SoC:** Unisoc SC9863A, 4× Cortex-A55 little + 4× Cortex-A55 big
- **Kernel:** vendor `4.14.199`, unchanged
- **Method:** device-tree OPP table patch only
- **Status:** validated and running

---

## 1. Result

| Cluster | Cores | Stock top OPP | This patch | Voltage |
|---|---|---:|---:|---:|
| little | 0–3 | 1.2 GHz | **1.6 GHz** | **1.25 V** |
| big | 4–7 | 1.6 GHz | **2.0 GHz** | **1.25 V** |

Live checks after flashing:

```text
/sys/devices/system/cpu/cpufreq/policy0/cpuinfo_max_freq = 1600000
/sys/devices/system/cpu/cpufreq/policy4/cpuinfo_max_freq = 2000000

policy0/scaling_available_frequencies =
  768000 884000 1000000 1100000 1200000 1600000

policy4/scaling_available_frequencies =
  768000 1050000 1225000 1400000 1500000 1600000 2000000
```

A 3-second all-core run held **2.0 GHz on cores 4–7** and **1.6 GHz on cores
0–3** for 100 % of the measurement window, at `vddcpu = 1,250,000 µV`.

> **Both are genuine new OPPs.** The stock little table tops out at **1.2 GHz**
> (about 1.10 V in the bin table this phone uses), and the stock big table tops
> out at **1.6 GHz**. The patch prepends 1.6 GHz / 1.25 V to the little tables
> and 2.0 GHz / 1.25 V to the big tables. Neither entry is a duplicate, and the
> live regulator reads 1,250,000 µV at the top OPP on both clusters.

---

## 2. Why the obvious patch does not work

The SC9863A cpufreq driver is not a simple "read `operating-points` and use it"
driver. Each cluster node has an `nvmem-cells` phandle pointing at a DVFS bin
value burned into the SoC efuse:

```dts
cpufreq_clus1: cpufreq-clus1 {
    cpufreq-cluster-id = <1>;
    nvmem-cells = <&dvfs_bin>;
    nvmem-cell-names = "dvfs_bin";
    ...
    operating-points   = <...>;
    operating-points-1 = <...>;
    operating-points-2 = <...>;
    operating-points-3 = <...>;
    operating-points-1-65 = <...>;
    operating-points-2-65 = <...>;
    operating-points-3-65 = <...>;
};
```

At probe time the driver reads the bin and appends `-<bin>` to the property
name. On this phone:

```text
cpu1_bin = 2   ->   the big cluster reads operating-points-2
```

The first 2.1 GHz attempt patched only `operating-points`. The kernel ignored
it, and the boot log showed OPP-removal warnings starting at 1.5 GHz: proof
that the parsed table was `operating-points-2`, whose first entry was 1.6 GHz,
not the 2.0 GHz value that had been added to the base table.

### The fix

Patch **all four** base tables in both clusters. This makes the image work
regardless of which bin a particular phone reports, and it keeps the change
portable across SM-A032F units.

Leave the `-65` tables alone. They are the thermal fallback used at or above
65 °C; changing them would remove a safety net rather than add performance.

---

## 3. The exact OPP values

### 3.1 Big cluster (`cpufreq-clus1`)

Prepend this pair to `operating-points`, `operating-points-1`,
`operating-points-2`, and `operating-points-3`:

```dts
/* 2,000,000 kHz, 1,250,000 uV */
<0x1e8480 0x1312d0 ...>
```

Decoded:

| Hex | Decimal | Meaning |
|---:|---:|---|
| `0x1e8480` | 2,000,000 | frequency in kHz |
| `0x1312d0` | 1,250,000 | voltage in µV |

The existing 1.6 GHz entry stays in the table. The new 2.0 GHz entry is simply
the first (highest) OPP.

### 3.2 Little cluster (`cpufreq-clus0`)

Prepend this pair to the same four tables:

```dts
/* 1,600,000 kHz, 1,250,000 uV */
<0x186a00 0x1312d0 ...>
```

| Hex | Decimal | Meaning |
|---:|---:|---|
| `0x186a00` | 1,600,000 | frequency in kHz |
| `0x1312d0` | 1,250,000 | voltage in µV |

### 3.3 Full patched lines

The values below are the actual v4 device tree. The `...` marks the unchanged
remainder of each stock table.

```dts
/* little cluster */
operating-points   = <0x186a00 0x1312d0 0x124f80 0x111e53 0x10c8e0 0x106738
                      0xf4240 0xfb01d 0xd7d20 0xee098 0xbb800 0xe04de>;
operating-points-1 = <0x186a00 0x1312d0 0x124f80 0x100590 0x10c8e0 0xf5aaa
                      0xf4240 0xea38f 0xd7d20 0xdc7d5 0xbb800 0xdbba0>;
operating-points-2 = <0x186a00 0x1312d0 0x124f80 0x10c8e0 0x10c8e0 0x101dfa
                      0xf4240 0xf66df 0xd7d20 0xe8b25 0xbb800 0xdbba0>;
operating-points-3 = <0x186a00 0x1312d0 0x124f80 0x118c30 0x10c8e0 0x10e14a
                      0xf4240 0x102a2f 0xd7d20 0xf4e75 0xbb800 0xe7ef0>;

/* big cluster */
operating-points   = <0x1e8480 0x1312d0 0x186a00 0x111e53 0x16e360 0x108bd7
                      0x155cc0 0x100590 0x12b128 0xf0537 0x100590 0xe1113
                      0xbb800 0xdbba0>;
operating-points-1 = <0x1e8480 0x1312d0 0x186a00 0x100590 0x16e360 0xf7f49
                      0x155cc0 0xeeccd 0x12b128 0xdf8a9 0x100590 0xdbba0
                      0xbb800 0xdbba0>;
operating-points-2 = <0x1e8480 0x1312d0 0x186a00 0x10c8e0 0x16e360 0x104299
                      0x155cc0 0xfb01d 0x12b128 0xebbf9 0x100590 0xdc7d5
                      0xbb800 0xdbba0>;
operating-points-3 = <0x1e8480 0x1312d0 0x186a00 0x118c30 0x16e360 0x1105e9
                      0x155cc0 0x10736d 0x12b128 0xf7f49 0x100590 0xe8b25
                      0xbb800 0xdbba0>;
```

The focused diff is in [`patches/cpu-2ghz-1.6ghz.diff`](../patches/cpu-2ghz-1.6ghz.diff).

---

## 4. Build and flash

The full boot-image procedure is in
[BUILD-AND-FLASH.md](BUILD-AND-FLASH.md). For a CPU-only image:

```sh
# Work from a copy of a known-good boot image.
python3 scripts/unpack_boot.py --image boot-stock.img --outdir work

# Decompile the FDT.
dtc -I dtb -O dts -o work/fdt.dts work/fdt.dtb

# Edit the two cpufreq nodes in work/fdt.dts using the lines in section 3.3.
$EDITOR work/fdt.dts

# Recompile and pack.
dtc -I dts -O dtb -o work/fdt-patched.dtb work/fdt.dts
python3 scripts/pack_dtbo.py \
  --image boot-stock.img \
  --fdt work/fdt-patched.dtb \
  --out boot-2ghz-little1.6.img

# Verify before flashing.
python3 scripts/verify_boot.py --image boot-2ghz-little1.6.img
```

If you prefer to edit the live FDT without a full decompile, `fdtput` can
replace one complete property at a time:

```sh
# Big cluster, bin-2 table (the one this phone uses). All values are required.
fdtput -t x work/fdt.dtb /cpufreq-clus1 operating-points-2 \
  0x1e8480 0x1312d0 \
  0x186a00 0x10c8e0 \
  0x16e360 0x104299 \
  0x155cc0 0xfb01d \
  0x12b128 0xebbf9 \
  0x100590 0xdc7d5 \
  0xbb800 0xdbba0
```

Repeat for the other three tables. The DTS edit is less error-prone because
you can see the whole table.

---

## 5. Verification

After reboot, run these as root:

```sh
# Cluster maxima and available frequencies.
for p in policy0 policy4; do
  echo "== $p"
  cat /sys/devices/system/cpu/cpufreq/$p/cpuinfo_max_freq
  cat /sys/devices/system/cpu/cpufreq/$p/scaling_available_frequencies
done

# Live voltage and temperature.
cat /sys/class/regulator/regulator.1/microvolts
cat /sys/class/thermal/thermal_zone1/temp

# Did the driver accept the new OPPs?
dmesg | grep -iE 'cpufreq|opp|bin' | tail -40
```

Expected:

- `policy4/cpuinfo_max_freq = 2000000`
- `policy0/cpuinfo_max_freq = 1600000`
- `2,000,000` is present in `policy4/scaling_available_frequencies`
- `vddcpu = 1250000` when the top OPP is active
- no `opp` removal warnings for the new frequency
- no under-voltage, stall, or thermal-critical messages

To prove the frequency is actually used (not just advertised), read the
residency counters before and after a short load:

```sh
# Snapshot
for c in 0 4; do
  echo "cpu$c"; cat /sys/devices/system/cpu/cpu$c/cpufreq/stats/time_in_state
done

# Load (example: one thread per core, 3 s)
su -c 'taskset -c 0-7 sh -c "for i in 0 1 2 3 4 5 6 7; do yes > /dev/null & done; sleep 3; kill %1 %2 %3 %4 %5 %6 %7 %8"'

# Snapshot again and compare the 2000000 / 1600000 lines.
```

The `time_in_state` interface is available on this vendor kernel and gives
exact per-frequency residency in 10 ms ticks.

---

## 6. Results

### 6.1 Throughput

Identical C workload, 3-second duration mode, screen off, performance governor:

| Configuration | Single core (cpu4) | All 8 cores | Speedup |
|---|---:|---:|---:|
| **2.0 GHz big / 1.6 GHz little** | **49.22 Miter/s** | **340.78 Miter/s** | 6.92× |
| 1.6 GHz big / 1.2 GHz little (capped) | 39.66 Miter/s | 277.17 Miter/s | 6.99× |
| Change | **+24.1 %** | **+22.9 %** | — |

The 1.6/1.2 comparison is a controlled cap on the same v4 image, not a
separate stock boot image. A repeat all-core run measured 336.18 Miter/s;
the 340.78 Miter/s run held the target OPPs for 100 % of the window.

Per-core efficiency is 86.5 % of the single-core rate (6.92× / 8 threads).
The heterogeneous ideal for 4×2.0 + 4×1.6 GHz is 354.4 Miter/s; the measured
340.78 Miter/s is **96.2 %** of that ideal.

### 6.2 Frequency, voltage, temperature

| Metric | 2.0 / 1.6 GHz | 1.6 / 1.2 GHz |
|---|---:|---:|
| Big clock | 2,000 MHz | 1,600 MHz |
| Little clock | 1,600 MHz | 1,200 MHz |
| `vddcpu` | 1,250,000 µV | 1,100,000 µV |
| All-core start → end temp | 43.6 → 67.6 °C | 47.1 → 57.8 °C |
| 65 °C thermal fallback in 3 s | reached at the very end | none |

A sustained 6.37 s all-core run produced 251.2 Miter/s after the little
cluster fell to 1.2 GHz and the big cluster to 1.6 GHz. This is the expected
behavior: the top OPP is a burst configuration, not a sustained one.

### 6.3 Power estimate

Using `P ∝ V² × f` for the same A55 cores:

| Domain | Voltage ratio | Frequency ratio | Dynamic power ratio |
|---|---:|---:|---:|
| Big | 1.25 / 1.10 = 1.136 | 2.0 / 1.6 = 1.25 | **1.61×** |
| Little | 1.25 / 1.10 = 1.136 | 1.6 / 1.2 = 1.333 | **1.72×** |
| Whole SoC, equal core weight | — | — | **≈1.67×** |

The overclock buys about **23 % more all-core throughput** for roughly **1.67×
the estimated dynamic power**, i.e. about **26 % lower performance per watt**.
The comparison is the same one used in section 6.1: 2.0/1.6 GHz versus a
1.6/1.2 GHz cap on the same image. This is an estimate from voltage and
frequency only. It ignores static leakage, regulator losses, and the fact that
the big cluster spends some time at lower OPPs in real workloads.

---

## 7. Thermal behavior and the vendor power HAL

The device tree contains a second set of tables with a `-65` suffix. The
driver switches to them when the cluster temperature reaches 65 °C:

| Table | Little fallback | Big fallback |
|---|---:|---:|
| `operating-points-*-65` | 1.2 GHz | 1.6 GHz |

This is why a long all-core load does not stay at 2.0/1.6 GHz. The fallback is
intentional and should be kept.

There is also a userspace component: the vendor power HAL can cap the big
cluster during sustained or normal-use scenarios. The 2.0 GHz OPP is therefore
**bursty** in practice. The benchmark's `performance` governor and explicit
`scaling_max_freq` writes bypass that cap for the measurement, but a game or a
long compile will see the thermal fallback.

Practical expectations:

- Short bursts (UI, app launch, camera, browser page load): 2.0 GHz is
  available and measurably faster.
- Sustained all-core work (video export, long compile, emulation): the phone
  settles at 1.6 GHz big / 1.2 GHz little and is roughly stock-fast.
- The phone runs about 10 °C hotter at the end of a 3 s all-core burst.

---

## 8. Tuning

Values below are for experiments. Only the 2.0/1.6 combination is validated by
this repository.

| Target | Frequency hex | Suggested voltage | Voltage hex | Notes |
|---:|---:|---:|---:|---|
| 1.8 GHz big | `0x1b7740` | 1.15 V | `0x118c30` | mild step, likely safe |
| 1.9 GHz big | `0x1cfde0` | 1.20 V | `0x124f80` | untested |
| **2.0 GHz big** | **`0x1e8480`** | **1.25 V** | **`0x1312d0`** | **validated** |
| 2.1 GHz big | `0x200b20` | 1.30 V | `0x13d620` | untested; v3 DTBO was rejected |
| 2.2 GHz big | `0x2191c0` | 1.35 V | `0x149970` | not recommended |
| 1.7 GHz little | `0x19f0a0` | 1.20 V | `0x124f80` | untested |
| **1.6 GHz little** | **`0x186a00`** | **1.25 V** | **`0x1312d0`** | **validated (stock little top is 1.2 GHz)** |

Rules for a new step:

1. Change one cluster at a time.
2. Add the new OPP to all four bin tables.
3. Keep the `-65` tables stock.
4. Verify the OPP appears and the voltage is what you intended.
5. Run a 3 s single-core test, then a 3 s all-core test, then a 60 s sustained
   test with temperature logging.
6. Roll back on any panic, stall, wrong result, or thermal-critical event.

The 2.1 GHz image in the history of this project is a cautionary tale: its DTBO
container was written little-endian, so the bootloader ignored it. The kernel
was probably never exercised at 2.1 GHz. If you retry 2.1 GHz, rebuild the
container with `scripts/pack_dtbo.py` and check the first four bytes are
`d7 b7 ab 1e`.

---

## 9. Gotchas

| Symptom | Cause | Fix |
|---|---|---|
| New frequency absent from `scaling_available_frequencies` | Patched only `operating-points`, but the efuse bin selects `operating-points-2` | Patch all four base tables |
| New frequency appears but voltage is wrong | The table contains two entries for that frequency and the kernel kept the other one | Check the order and remove unintended duplicates |
| 2.0 GHz appears but is never used | Vendor power HAL cap or thermal fallback | Use `performance` governor and a cool device for the test; expect fallback under sustained load |
| Little cluster stays at 1.2 GHz | The four little bin tables were not patched, or the DTBO was ignored | Patch all four little tables and verify the DTBO magic |
| Phone boot-loops after flash | Bad DTBO or wrong `dtb_size` | Reflash the stock/v4 boot image from the host |
| `time_in_state` shows 1.2 GHz during a "1.6 GHz" run | 65 °C thermal table | Cool the device, shorten the run, or accept the fallback |

---

## 10. Rollback

CPU rollback is the same as any boot-image rollback:

```sh
# On the phone, as root. Replace the path with your own rollback image.
su -c 'dd if=/sdcard/boot-2ghz-little1.6-v4.img of=/dev/block/by-name/boot bs=1M conv=fsync'
sync
reboot
```

Keep at least two known-good images:

1. the stock boot image you dumped before the first flash;
2. the last validated overclock image (CPU-only or CPU+GPU).

See [BUILD-AND-FLASH.md](BUILD-AND-FLASH.md#10-rollback) for the full recovery
tree, including the download-mode/Odin fallback.

---

## 11. Checklist

- [ ] Bootloader unlocked, root working, download mode tested.
- [ ] Stock boot partition dumped and copied off the phone.
- [ ] Boot image SHA-256 recorded.
- [ ] FDT extracted and decompiled.
- [ ] All four big-cluster tables patched with `0x1e8480 0x1312d0`.
- [ ] All four little-cluster tables patched with `0x186a00 0x1312d0`.
- [ ] `-65` tables unchanged.
- [ ] DTBO magic is `d7 b7 ab 1e`.
- [ ] `dtb_size` updated and container fits.
- [ ] Kernel and ramdisk hashes unchanged.
- [ ] Image verified before flashing.
- [ ] Only the `boot` partition written.
- [ ] `policy4/cpuinfo_max_freq = 2000000`.
- [ ] `vddcpu = 1250000` at the top OPP.
- [ ] 3 s single and all-core benchmarks match the expected range.
- [ ] 60 s sustained test shows thermal fallback, not instability.
- [ ] Rollback image still available on the phone and on the host.
