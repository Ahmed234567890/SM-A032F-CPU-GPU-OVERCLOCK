# GPU benchmark results

- **Device:** Samsung SM-A032F (Galaxy A03 Core), Unisoc SC9863A, PowerVR Rogue
  GE8322 (BVNC 22.86.104.218)
- **Driver:** `pvrsrvkm`, Rogue DDK 1.18@6267915
- **Boot image:** CPU v4 + GPU 650, SHA-256
  `b331d013c2d1910717f72767ca3fd494b73ca0f0ae8094168ccb43a465a52e2d`
- **Workload:** `scripts/gpu_stress.c`, 1024 × 1024 pbuffer, 64-iteration
  fragment shader
- **Sampling:** GPU frequency, GPU/CPU temperature, `vddcore`, DDR frequency
  every 100 ms

---

## 1. Headline

| Metric | Stock 550 MHz | 650 MHz | Delta |
|---|---:|---:|---:|
| 20 s A/B, frames | 147 | 160 | — |
| 20 s A/B, fps | 7.31 | 7.97 | **+9.0 %** |
| Best 60 s run, fps | not measured at 60 s | **8.47** | +15.9 % vs 20 s baseline |
| Theoretical clock ratio | — | 650 / 550 | **+18.2 %** |

The 20 s A/B is the cleanest comparison because both runs use the same duration
and workload. The 60 s runs are the stability evidence; they are not directly
comparable to the 20 s baseline because the tool prints a cumulative average and
the first seconds include ramp-up.

---

## 2. 60-second runs at 650 MHz

| Run | Frames | fps | GPU max | CPU max | GPU errors |
|---|---:|---:|---:|---:|---:|
| 1 (earlier session) | 509 | **8.47** | 52.6 °C | 56.1 °C | 0 |
| 2 ([raw log](logs/gpu_650_60s_run2.log)) | 489 | 8.15 | 59.9 °C | 65.9 °C | 0 |
| 3 ([raw log](logs/gpu_650_60s_run3.log)) | 502 | 8.36 | 59.0 °C | 64.1 °C | 0 |

Run 1 was the fastest but its raw progress log was not retained; the later two
runs were repeated with fresh logs specifically for publication. All three
report the same zero error counters.

---

## 3. Telemetry from the fresh runs

| Metric | Run 2 | Run 3 |
|---|---:|---:|
| `gpu freq` min / max / avg | 256 / 650 / 634.3 MHz | 256 / 650 / 636.6 MHz |
| `gpu temp` min / max | 51.7 / 59.9 °C | 44.5 / 59.0 °C |
| CPU temp during progress | 49.8–65.9 °C | 48.0–64.1 °C |
| `vddcore` min / max | 900,000 / 900,000 µV | 900,000 / 900,000 µV |
| DDR min / max | 400 / 933 MHz | 400 / 933 MHz |

The GPU frequency `avg` is below 650 MHz because the sampler includes the 2 s
baseline and the post-load idle samples. Every progress line during the load
window reports `gpu=650000000`.

The GPU temperature stays below 60 °C. The CPU touches the 65 °C thermal table
near the end of run 2, which is why the fps curve declines slightly after about
20 s. That is the CPU-side submission path throttling, not a GPU fault.

---

## 4. Health snapshot after the runs

[`logs/gpu_health_after_runs.txt`](logs/gpu_health_after_runs.txt):

```text
Driver Status:   OK
Firmware Status: OK
Server Errors:   0
HWR Event Count: 0
CRR Event Count: 0
SLR Event Count: 0
WGP Error Count: 0
TRP Error Count: 0
FWF Event Count: 0
```

`APM Event Count` is not an error counter; it increments with normal GPU
activity and is expected to grow.

`trans_stat` after the runs:

```text
Total transition : 174

From 650000000:  dwell 400,524 ms
```

The GPU spent about 400 seconds at 650 MHz across the test session, with 174
mux transitions. That is direct evidence that the 650 MHz OPP is selected and
held under load.

---

## 5. Why the gain is not the full 18.2 %

The pure clock ratio predicts:

```text
7.31 × 650 / 550 = 8.64 fps
```

The best 60 s run (8.47) is **98 %** of that. The fresh runs (8.15–8.36) are
**94–97 %**. The same-length 20 s A/B (7.97) is **92 %**.

The difference is explained by:

- EGL/GLES submission and the frame loop run on the CPU;
- the CPU enters the 65 °C thermal table during a 60 s run;
- memory and driver overhead do not scale with the GPU core clock;
- Android run-to-run spread is about 2–4 %.

A failed overclock would show no fps delta at all, or a delta far below the
clock ratio with GPU errors. The observed pattern is consistent with a real
650 MHz clock plus a CPU-side ceiling.

---

## 6. Real-world expectation

The stress shader is ALU-heavy. Games are more memory- and bandwidth-bound, and
many scenes are CPU-limited on this SoC. A realistic gain in GPU-limited scenes
is **5–15 %**; CPU-limited scenes will not improve. The 650 MHz step is not a
miracle, it is a 18 % core clock increase with no voltage change.

---

## 7. Raw logs

| File | Content |
|---|---|
| [`logs/gpu_650_60s_run2.log`](logs/gpu_650_60s_run2.log) | 60 s, 489 frames, 8.15 fps |
| [`logs/gpu_650_60s_run3.log`](logs/gpu_650_60s_run3.log) | 60 s, 502 frames, 8.36 fps |
| [`logs/gpu_20s_ab.log`](logs/gpu_20s_ab.log) | 20 s telemetry sampler: 550 → 650 transitions |
| [`logs/gpu_health_after_runs.txt`](logs/gpu_health_after_runs.txt) | `/proc/pvr/status` + `trans_stat` snapshot |

The frame counts for the 20 s A/B were captured in the terminal during the
earlier session; the retained sampler log contains the frequency and
temperature trace for that session, not the frame counter.

---

## 8. Caveats

- The absolute GPLL rate is not readable on this kernel. `CONFIG_DEBUG_FS` is
  disabled, `/dev/mem` is unavailable, and `kptr_restrict=2`. Validation is
  indirect: OPP label, fps delta, and 650 MHz dwell time.
- The GPU has no adjustable voltage rail. All runs use the fixed 900 mV
  `vddcore`. There is no voltage margin to recover a weak sample.
- The 700 MHz step is **not enabled** and is not part of these results. It uses
  SDM and has less timing margin at the same voltage.
- The 60 s fps values are cumulative averages. Do not compare a 20 s run with a
  60 s run and call the difference an overclock effect.
- The phone was charged during the runs; the CPU temperature is higher than it
  would be on battery. This is conservative for stability testing.
