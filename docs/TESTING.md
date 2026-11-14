# Testing and validation

This document describes how the numbers in [results/](../results/) were
produced and how to reproduce them on your own device. The goal is not just
"it booted": each change is validated with a controlled workload, a frequency
residency check, a voltage/temperature check, and an abort criterion.

---

## 1. Test philosophy

1. **One change at a time.** CPU first, then GPU. If something fails, you know
   which change caused it.
2. **Same workload, same duration.** A 20 s run is compared with a 20 s run;
   a 60 s run with a 60 s run. The GPU tool prints a cumulative average, so
   mixing durations is misleading.
3. **Cold start.** Let the device cool to about 42–45 °C before a run. A hot
   start measures the thermal table, not the overclock.
4. **Screen off, charger connected, background apps stopped.** Android
   background activity adds 2–4 % noise.
5. **Record the raw output.** Keep the `.out`/`.log` file, not just the headline
   number.
6. **Stop at the first anomaly.** A single GPU error counter, a wrong checksum,
   or a thermal-critical event is a fail, not a data point to average away.

---

## 2. Pre-flight

```sh
# Root and kernel.
su -c 'id'
uname -a

# Boot state.
getprop ro.boot.verifiedbootstate
getprop ro.boot.flash.locked

# Battery and temperature.
dumpsys battery | grep -E 'level|temperature'
cat /sys/class/thermal/thermal_zone1/temp

# CPU policies.
for p in policy0 policy4; do
  echo "== $p"
  cat /sys/devices/system/cpu/cpufreq/$p/scaling_governor
  cat /sys/devices/system/cpu/cpufreq/$p/scaling_min_freq
  cat /sys/devices/system/cpu/cpufreq/$p/scaling_max_freq
done

# GPU.
su -c 'cat /sys/class/devfreq/60000000.gpu/cur_freq'
su -c 'cat /sys/class/devfreq/60000000.gpu/governor'
su -c 'cat /proc/pvr/status' | head -20
```

Requirements:

| Item | Target |
|---|---|
| Battery | ≥ 50 %, charging |
| CPU temperature before a run | ≤ 45 °C |
| GPU temperature before a run | ≤ 45 °C |
| Screen | off |
| Background apps | force-stopped |
| Thermal zone 1 | CPU cluster sensor |
| Root | required for `performance` and sysfs reads |

Cool-down helper (runs on the phone as root):

```sh
sh bench_cool.sh 42000
```

It turns the screen off and waits until `thermal_zone1` is at or below 42 °C,
up to 10 minutes.

---

## 3. CPU benchmark

### 3.1 Build

The same source is compiled twice:

```sh
clang -O2 -pthread -o cpu_bench_single cpu_bench.c -DDEFAULT_MODE='"single"'
clang -O2 -pthread -o cpu_bench_all    cpu_bench.c -DDEFAULT_MODE='"all"'
```

### 3.2 Workload

A serial 32-bit integer chain with six multiplies per iteration plus
shifts/xors. It is fully dependent, cannot be vectorized, and has no memory
footprint, so the measured time is CPU execution time.

- Single mode pins one thread to cpu4 (first big core).
- All mode starts one thread per online CPU and pins thread *i* to cpu *i*.
- `--duration 3` makes every thread process 1M-iteration chunks until a shared
  3-second deadline, so all cores stop together.
- The binary applies `performance` and the requested cluster caps, waits
  100 ms, then restores `schedutil`.
- It sets nice `-20` (root) to reduce preemption.
- `time_in_state` snapshots are taken immediately before and after the timed
  region.

### 3.3 Run

```sh
su -c 'sh bench_cool.sh 42000'

su -c 'setsid ./cpu_bench_single --duration 3 \
  --little-max 1600000 --big-max 2000000 \
  > bench_single.out 2>&1 < /dev/null &'

su -c 'setsid ./cpu_bench_all --duration 3 \
  --little-max 1600000 --big-max 2000000 \
  > bench_all.out 2>&1 < /dev/null &'
```

For the stock-equivalent comparison, cap the same binary:

```sh
su -c 'setsid ./cpu_bench_single --duration 3 \
  --little-max 1200000 --big-max 1600000 \
  > bench_stock_single.out 2>&1 < /dev/null &'

su -c 'setsid ./cpu_bench_all --duration 3 \
  --little-max 1200000 --big-max 1600000 \
  > bench_stock_all.out 2>&1 < /dev/null &'
```

### 3.4 Expected numbers

| Configuration | Single (cpu4) | All 8 cores | Speedup |
|---|---:|---:|---:|
| 2.0 GHz big / 1.6 GHz little | **49.22 Miter/s** | **340.78 Miter/s** | 6.92× |
| 1.6 GHz big / 1.2 GHz little | 39.66 Miter/s | 277.17 Miter/s | 6.99× |
| Change | +24.1 % | +22.9 % | — |

Acceptance range (Android noise):

| Metric | Expected | Acceptable | Investigate |
|---|---:|---:|---:|
| Single, 2.0 GHz | 49.2 | 47.5–50.5 | < 46 |
| All-core, 2.0/1.6 | 340.8 | 330–345 | < 320 |
| All-core, 1.6/1.2 | 277.2 | 270–280 | < 265 |

### 3.5 What to inspect

- `RESULT ... throughput_mips=...` line.
- `freq cpu4 avg_mhz=... 2000000:100%` for the single run.
- `freq cpu0..3 avg_mhz=... 1600000:100%` for the all-core run.
- `vddcpu_uv before=... after=...` should be 1,250,000 during the top OPP.
- `temp_millic before=... after=...` should be below the 65 °C fallback for a
  3 s run (it may touch it at the very end).
- `procstat` idle should be < 1 %.

### 3.6 Common failure modes

| Symptom | Meaning |
|---|---|
| `scaling_max_freq` write fails silently | not root, or the OPP is not in the table |
| Frequency stays at 1.6 GHz | patched the wrong bin table |
| `vddcpu` stays at 1.1 V at 1.6 GHz | the 1.6 GHz OPP was not selected (wrong table or thermal fallback) |
| Little cluster falls to 1.2 GHz early | 65 °C thermal table; cool the device or shorten the run |
| Throughput below 320 Miter/s | thermal fallback, background load, or a bad patch |
| Kernel panic / oops | unstable OPP; roll back |

---

## 4. GPU stress

### 4.1 Build

```sh
clang -O2 -pthread -o gpu_stress gpu_stress.c -ldl
```

### 4.2 Workload

`gpu_stress.c` creates an EGL pbuffer and a GLES2 context, draws a 1024 × 1024
full-screen quad with a heavy fragment shader (64 iterations of
`sin`/`cos`/`length`), and samples GPU devfreq frequency, GPU/CPU temperature,
`vddcore`, and DDR frequency every 100 ms. It renders to its own pbuffer and
only reads sysfs/procfs.

### 4.3 Run

```sh
su -c './gpu_stress -d 60 -s 1024 -i 64 -b 2'
```

Options:

| Option | Meaning | Default |
|---|---|---|
| `-d N` | load duration in seconds | 15 |
| `-s N` | pbuffer size (N × N) | 1024 |
| `-i N` | shader iterations | 64 |
| `-b N` | baseline seconds before load | 0 |

The tool prints a progress line every ~1 s and a summary at the end.

### 4.4 Expected numbers

| Configuration | Duration | Frames | fps | Notes |
|---|---:|---:|---:|---|
| Stock 550 MHz | 20 s | 147 | 7.31 | baseline |
| 650 MHz | 20 s | 160 | 7.97 | +9.0 % |
| 650 MHz | 60 s | 489–509 | 8.15–8.47 | three runs |
| 650 MHz theoretical | — | — | 8.64 | 7.31 × 650/550 |

A 60 s 650 MHz run should show:

```text
gpu freq: min=256000000 max=650000000 avg≈634–637 MHz
gpu temp: min≈45 max≈59–60 C
vddcore: min=900000 max=900000 uV
ddr freq: min=400 max=933 MHz
```

The `avg` is below 650 because the sampler includes the baseline/idle samples
before and after the load. The progress lines during the load should all show
`gpu=650000000`.

### 4.5 GPU health checks

Before and after each run:

```sh
su -c 'cat /proc/pvr/status'
su -c 'cat /sys/class/devfreq/60000000.gpu/trans_stat'
su -c 'cat /proc/pvr/gpu00/debug_dump' | grep -i 'RGX DVFS'
su -c 'dmesg' | grep -iE 'pvr|rgx|gpu|gpll|clk' | tail -40
```

Acceptance:

| Counter | Pass | Fail |
|---|---:|---:|
| `Driver Status` | OK | anything else |
| `Firmware Status` | OK | anything else |
| `HWR Event Count` | 0 | > 0 |
| `CRR Event Count` | 0 | > 0 |
| `SLR Event Count` | 0 | > 0 |
| `WGP Error Count` | 0 | > 0 |
| `TRP Error Count` | 0 | > 0 |
| `FWF Event Count` | 0 | > 0 |
| `APM Event Count` | any | not an error counter |

`trans_stat` should show a growing 650 MHz dwell time and transitions into the
650 MHz state. After the three runs in this project: 174 total transitions and
400,524 ms of 650 MHz dwell.

### 4.6 Stability sequence

1. Cool to ≤ 45 °C.
2. 20 s run, check counters.
3. Cool to ≤ 45 °C.
4. 60 s run, check counters.
5. Repeat 60 s twice more with a cool-down between runs.
6. Only after three clean 60 s runs, consider the next frequency step.

The 650 MHz configuration passed this sequence. The 700 MHz step is not enabled.

### 4.7 Common failure modes

| Symptom | Meaning |
|---|---|
| `max_freq=650000000` but fps ≈ 7.3 | GPLL did not move; check `assigned-clock-rates` |
| `max_freq=550000000` | OPP property not replaced, or old DTBO still active |
| `max_freq=650000000` and fps > 8.7 | suspiciously high; verify the workload and the PLL target |
| GPU temp > 70 °C | abort, cool, improve the test environment |
| HWR/CRR/FWF counters increase | timing failure; roll back |
| App crash or visual corruption | timing failure; roll back |
| `trans_stat` never enters 650 MHz | scene mapping or governor issue |

---

## 5. RAM / DDR stress

The DDR is **not** overclocked in this project. The stress test exists to show
that 933 MHz is the top OPP and that the memory path is healthy.

### 5.1 Build and run

```sh
clang -O3 -pthread -o ram_stress ram_stress.c

su -c 'nice -n -20 ./ram_stress -t 8 -s 16 -d 2 -b 1'
```

The tool allocates one buffer per thread, faults it in, and runs three phases:
`write` (`memset`), `read` (64-bit sum), and `copy` (`memcpy` between halves).
A sampler reads the real DDR frequency from
`/sys/class/devfreq/scene-frequency/sprd-governor/ddrinfo_cur_freq` every
100 ms, plus AP/CP votes, CPU frequency, temperature, and `MemAvailable`.

It never writes DDR sysfs, never forces a frequency, and never touches storage.

### 5.2 Expected numbers

| Metric | Value |
|---|---|
| DDR min / max | 400 / **933 MHz** |
| DDR average | 840–858 MHz |
| Samples at 933 MHz | 79–84 % |
| Write bandwidth | 6.66–6.68 GB/s |
| Read bandwidth | 5.73–5.74 GB/s |
| Copy bandwidth (R+W) | 5.84–5.87 GB/s |
| `scaling_force_ddr_freq` | 0 |
| CPU temperature | 78–80 °C (charging, sustained) |

Theoretical peak at 933 MHz with a 32-bit bus:

```text
933 MHz × 2 (DDR) × 4 bytes = 7.464 GB/s
```

Write reaches ~89 % of peak, read ~77 %, copy ~79 %.

### 5.3 Why not 1000 MHz

- 933 MHz is the top entry in the platform's DDR DFS table.
- The table is owned by the PM co-processor firmware, not the AP kernel.
- The AP-side `sprd-governor` also hardcodes 933 MHz.
- 1000 MHz means LPDDR3-2000, above the platform's validated 1866 MT/s.
- LPDDR3 has no ECC; running outside the timing margin can cause silent data
  corruption, not just a crash.
- `pm_sys` is a critical boot component; a bad image can prevent boot.

The theoretical gain is only ~7.2 % in peak bandwidth, and the measured
read/copy bandwidth is already fabric-limited. Leave the DDR at 933 MHz.

---

## 6. Data collection

A complete test record should contain:

| File | Content |
|---|---|
| `cpu_single.out` | single-core benchmark output |
| `cpu_all.out` | all-core benchmark output |
| `gpu_60s_run1.log` | GPU stress progress + summary |
| `gpu_60s_run2.log` | second run |
| `gpu_60s_run3.log` | third run |
| `pvr_status_before.txt` | `/proc/pvr/status` before the runs |
| `pvr_status_after.txt` | `/proc/pvr/status` after the runs |
| `trans_stat_before.txt` | `trans_stat` before |
| `trans_stat_after.txt` | `trans_stat` after |
| `dmesg_tail.txt` | kernel log tail |
| `thermal.csv` | timestamped temperature samples |

Useful commands:

```sh
su -c 'cat /proc/pvr/status' > pvr_status_after.txt
su -c 'cat /sys/class/devfreq/60000000.gpu/trans_stat' > trans_stat_after.txt
su -c 'dmesg' | tail -200 > dmesg_tail.txt

# 1 Hz thermal log (example).
while true; do
  echo "$(date +%s) $(cat /sys/class/thermal/thermal_zone1/temp)"
  sleep 1
done > thermal.csv
```

Do not publish your own logs without checking them for device identifiers,
paths, IP addresses, or usernames.

---

## 7. Abort criteria

### CPU

- kernel panic, oops, stall, or watchdog reset;
- under-voltage or regulator fault in `dmesg`;
- wrong benchmark checksum;
- `vddcpu` not at the intended value for the active OPP;
- temperature above 80 °C or a thermal-critical event;
- frequency residency not at the target OPP for a 3 s cool run.

### GPU

- any HWR/CRR/SLR/WGP/TRP/FWF counter increase;
- GPU hang, app crash, or visible rendering corruption;
- GPU temperature above 70 °C;
- fps below the 550 MHz baseline;
- GPLL or clock-framework error in `dmesg`;
- boot loop or no display after flashing.

### RAM

- data mismatch or checksum failure;
- kernel panic or filesystem error;
- DDR thermal event;
- any storage I/O error during the run.

---

## 8. Result template

Copy this into your own notes or a pull request:

```text
Device: SM-A032F
Firmware: A032FXXS9CZA2
Kernel: 4.14.199-...
Boot image SHA-256: ...
CPU patch: 2.0 GHz big / 1.6 GHz little
GPU patch: 650 MHz
Ambient: ... °C
Battery: ... %

CPU single: ... Miter/s  (target 49.22)
CPU all:    ... Miter/s  (target 340.78)
CPU vddcpu: ... uV
CPU temp:   ... -> ... °C

GPU 20 s: ... frames / ... fps
GPU 60 s run 1: ... frames / ... fps, GPU ... °C, CPU ... °C
GPU 60 s run 2: ... frames / ... fps, GPU ... °C, CPU ... °C
GPU 60 s run 3: ... frames / ... fps, GPU ... °C, CPU ... °C
GPU errors: HWR ... CRR ... SLR ... WGP ... TRP ... FWF ...
trans_stat 650 dwell: ... ms

DDR max: ... MHz
Write/read/copy: ... / ... / ... GB/s

Rollback image: ... (SHA-256 ...)
Notes:
```

---

## 9. Reproducibility caveats

- The userspace is 32-bit (`armeabi-v7a`), so the benchmark binaries are ARMv7.
  A 64-bit benchmark is not possible on this ROM without a 64-bit userspace.
- Results include real Android background activity. Screen-off and nice `-20`
  reduce it, but `/proc/stat` still shows a few percent IRQ and idle time.
- The all-core CPU figures are 3 s bursts from a cold start. Longer loads are
  limited by the 65 °C thermal table.
- The GPU tool's fps is a cumulative average. A 20 s run includes more startup
  overhead than a 60 s run, so compare equal durations.
- Run-to-run spread on Android is about 2–4 %. A single fast run is not proof
  of a stable overclock; three clean runs are.
- The GPU's absolute GPLL rate is not readable on this kernel. The evidence is
  the OPP label, the fps delta, and the 650 MHz dwell time, not a direct clock
  read.
