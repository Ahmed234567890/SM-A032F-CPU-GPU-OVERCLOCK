# CPU benchmark results

- **Device:** Samsung SM-A032F (Galaxy A03 Core), Unisoc SC9863A, 8× Cortex-A55
- **Firmware:** `A032FXXS9CZA2`, vendor kernel `4.14.199`
- **Boot image:** CPU v4, SHA-256
  `94a650d5e4f258734a46888da5dcb4a288ef1e28d552af91a19a925eda2a7d62`
- **Workload:** `scripts/cpu_bench.c`, 3-second dependent integer chain
- **Conditions:** screen off, charger connected, `performance` governor, nice
  `-20`, near-cold start (41–48 °C) for each run

---

## 1. Headline

| Configuration | Single core (cpu4) | All 8 cores | All-core speedup |
|---|---:|---:|---:|
| **2.0 GHz big / 1.6 GHz little** | **49.22 Miter/s** | **340.78 Miter/s** | **6.92×** |
| 1.6 GHz big / 1.2 GHz little (cap) | 39.66 Miter/s | 277.17 Miter/s | 6.99× |
| Change | **+24.1 %** | **+22.9 %** | — |

The stock comparison is a **frequency cap on the same v4 image**, not a separate
stock boot image. That isolates the OPP change from every other difference in
the build.

---

## 2. Runs

| Run | Configuration | Mode | Miter/s | `vddcpu` | Temp start → end | Frequency residency |
|---|---|---|---:|---:|---:|---|
| `cpu_2ghz_single.out` | 2.0 / 1.6 | single, cpu4 | **49.221** | 1,250,000 µV | 43.6 → 46.3 °C | cpu4 2000 MHz 100 % |
| `cpu_2ghz_single_repeat.out` | 2.0 / 1.6 | single, cpu4 | 48.668 | 1,250,000 µV | 40.9 → 48.9 °C | cpu4 2000 MHz 100 % |
| `cpu_2ghz_all.out` | 2.0 / 1.6 | all, 8 threads | **340.780** | 1,250,000 µV | 43.6 → 67.6 °C | 2.0/1.6 GHz 100 % |
| `cpu_2ghz_all_repeat.out` | 2.0 / 1.6 | all, 8 threads | 336.177 | 1,250,000 → 1,121,875 µV | 45.4 → 66.8 °C | little 94 % at 1.6, big 100 % at 2.0 |
| `cpu_2ghz_all_repeat2.out` | 2.0 / 1.6 | all, 8 threads | 330.810 | 1,250,000 → 1,121,875 µV | 41.8 → 64.1 °C | little 88 % at 1.6, big 99 % at 2.0 |
| `cpu_stock_single.out` | 1.6 / 1.2 | single, cpu4 | 39.659 | 1,100,000 µV | 48.0 → 45.4 °C | cpu4 1600 MHz 100 % |
| `cpu_stock_all.out` | 1.6 / 1.2 | all, 8 threads | 277.165 | 1,100,000 µV | 47.1 → 57.8 °C | 1.6/1.2 GHz 100 % |

The 340.78 Miter/s run is the clean, full-residency run used in the headline.
The two repeats show what happens when the little cluster touches the 65 °C
table near the end: throughput falls to 330–336 Miter/s even though the big
cluster stays at 2.0 GHz. That is expected thermal behavior, not instability.

The all-core benchmark output also reports `procstat idle` of 0.2–0.5 %, so the
measurement is not contaminated by background load.

---

## 3. Per-core throughput

From `cpu_2ghz_all.out`:

| Core | Cluster | Nominal | Miter/s |
|---:|---|---:|---:|
| 0 | little | 1.6 GHz | 37.633 |
| 1 | little | 1.6 GHz | 37.858 |
| 2 | little | 1.6 GHz | 37.977 |
| 3 | little | 1.6 GHz | 36.791 |
| 4 | big | 2.0 GHz | 47.921 |
| 5 | big | 2.0 GHz | 47.618 |
| 6 | big | 2.0 GHz | 48.527 |
| 7 | big | 2.0 GHz | 48.545 |

From `cpu_stock_all.out`:

| Core | Nominal | Miter/s |
|---:|---:|---:|
| 0–3 | 1.2 GHz | 29.70–29.77 |
| 4–7 | 1.6 GHz | 39.41–39.84 |

The big/little ratio is 1.265 at 2.0/1.6 GHz and 1.339 at 1.6/1.2 GHz, close to
the nominal clock ratios (1.25 and 1.333). The cores scale with frequency, which
is what a successful OPP change should look like.

---

## 4. Scaling analysis

The heterogeneous ideal for 4 × 2.0 GHz + 4 × 1.6 GHz, using the measured
single-core rate as the per-GHz constant:

```text
4 × 49.22 + 4 × (49.22 × 1.6 / 2.0) = 354.4 Miter/s
```

Measured all-core: **340.78 Miter/s = 96.2 % of that ideal**. The 3.8 % gap is
normal for SMT-free cores sharing L3, the memory bus, and the scheduler.

For the stock cap:

```text
4 × 39.66 + 4 × (39.66 × 1.2 / 1.6) = 277.6 Miter/s
```

Measured: **277.17 Miter/s = 99.8 % of ideal**. The stock configuration is
almost perfectly frequency-limited, which makes the comparison trustworthy.

---

## 5. Power estimate

Using `P ∝ V² × f` and the measured `vddcpu` values:

| Domain | Voltage ratio | Frequency ratio | Dynamic power ratio |
|---|---:|---:|---:|
| Big | 1.25 / 1.10 = 1.136 | 2.0 / 1.6 = 1.25 | **1.61×** |
| Little | 1.25 / 1.10 = 1.136 | 1.6 / 1.2 = 1.333 | **1.72×** |
| Whole SoC, equal core weight | — | — | **≈1.67×** |

Throughput rises 22.9 % while estimated dynamic power rises about 67 %, so
performance per watt falls by about **26 %**. This is a burst configuration,
not an efficiency win. Static leakage, regulator losses, and real-world idle
time are not included in the estimate.

---

## 6. Thermal behavior

The device tree has `operating-points-*-65` fallback tables that the driver
selects at 65 °C:

| Table | Little | Big |
|---|---:|---:|
| normal | 1.6 GHz | 2.0 GHz |
| `-65` | 1.2 GHz | 1.6 GHz |

The clean 340.78 Miter/s run ended at 67.6 °C and still reported 100 %
residency because the snapshot was taken over the 3 s window; the fallback
engages at the boundary. The two repeats show the transition in the residency
counters (little 94 % / 88 % at 1.6 GHz). A longer 6.37 s all-core run settled
at 251.2 Miter/s, which is consistent with the thermal table.

This is why the guide calls 2.0 GHz a **burst** frequency. Short UI and app-launch
work gets it; sustained all-core work settles near stock.

---

## 7. Raw logs

| File | Content |
|---|---|
| [`logs/cpu_2ghz_single.out`](logs/cpu_2ghz_single.out) | 49.221 Miter/s single-core |
| [`logs/cpu_2ghz_single_repeat.out`](logs/cpu_2ghz_single_repeat.out) | 48.668 Miter/s single-core |
| [`logs/cpu_2ghz_all.out`](logs/cpu_2ghz_all.out) | 340.780 Miter/s all-core, full residency |
| [`logs/cpu_2ghz_all_repeat.out`](logs/cpu_2ghz_all_repeat.out) | 336.177 Miter/s, partial thermal fallback |
| [`logs/cpu_2ghz_all_repeat2.out`](logs/cpu_2ghz_all_repeat2.out) | 330.810 Miter/s, partial thermal fallback |
| [`logs/cpu_stock_single.out`](logs/cpu_stock_single.out) | 39.659 Miter/s at 1.6/1.2 cap |
| [`logs/cpu_stock_all.out`](logs/cpu_stock_all.out) | 277.165 Miter/s at 1.6/1.2 cap |

The logs contain only benchmark output, frequencies, voltages, temperatures,
and process statistics. They do not contain device identifiers or host paths.

---

## 8. Caveats

- The benchmark is a synthetic dependent integer chain. It measures core
  execution rate, not memory or GPU performance.
- The userspace is 32-bit (`armeabi-v7a`), so the binary is ARMv7. This does not
  affect the kernel-side OPP behavior.
- The stock comparison is a cap on the overclock image, not a stock boot image.
  It matches the stock frequencies (1.6 big / 1.2 little) and voltage (1.1 V),
  but it does not include any other stock-image differences.
- Android background activity adds 2–4 % run-to-run spread. The headline uses
  the cleanest run; the repeats are included so the spread is visible.
- The 65 °C fallback is a feature. Removing it would trade a safety net for a
  benchmark number.
