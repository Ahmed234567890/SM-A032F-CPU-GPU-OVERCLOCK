# Results

All numbers were produced on the device described in the top-level README, with
the boot image hashes listed in each report. Raw logs are committed next to the
summaries so the claims can be audited.

| Report | Headline |
|---|---|
| [CPU-RESULTS.md](CPU-RESULTS.md) | 49.22 Miter/s single, 340.78 Miter/s all-core; +24.1 % / +22.9 % vs 1.6/1.2 GHz |
| [GPU-RESULTS.md](GPU-RESULTS.md) | 650 MHz: 8.15–8.47 fps over 3 × 60 s, zero GPU error counters; 20 s A/B +9.0 % |
| [RAM-RESULTS.md](RAM-RESULTS.md) | 933 MHz DDR, 6.66–6.68 GB/s write, 5.73–5.74 GB/s read; no DDR overclock |

## Summary table

| Domain | Stock | This guide | Measured delta | Status |
|---|---|---|---|---|
| CPU big (4–7) | 1.6 GHz | **2.0 GHz @ 1.25 V** | +24.1 % single-core | validated |
| CPU little (0–3) | 1.2 GHz | **1.6 GHz @ 1.25 V** | contributes to +22.9 % all-core | validated |
| GPU | 550 MHz | **650 MHz** | +9.0 % same-length A/B; up to +15.9 % 60 s | 3 × 60 s clean |
| DDR | 933 MHz | 933 MHz | none | characterized |

## Log index

| File | Content |
|---|---|
| `logs/cpu_2ghz_single.out` | 49.221 Miter/s, 2.0/1.6 GHz |
| `logs/cpu_2ghz_single_repeat.out` | 48.668 Miter/s |
| `logs/cpu_2ghz_all.out` | 340.780 Miter/s, full residency |
| `logs/cpu_2ghz_all_repeat.out` | 336.177 Miter/s |
| `logs/cpu_2ghz_all_repeat2.out` | 330.810 Miter/s |
| `logs/cpu_stock_single.out` | 39.659 Miter/s at 1.6/1.2 GHz |
| `logs/cpu_stock_all.out` | 277.165 Miter/s at 1.6/1.2 GHz |
| `logs/gpu_650_60s_run2.log` | 489 frames / 8.15 fps |
| `logs/gpu_650_60s_run3.log` | 502 frames / 8.36 fps |
| `logs/gpu_20s_ab.log` | 20 s frequency/temperature telemetry |
| `logs/gpu_health_after_runs.txt` | `/proc/pvr/status` + `trans_stat` |
| `logs/ram_probe.out` | 3 s memory phases |
| `logs/ram_stress_confirm.out` | 2 s memory phases |

## Reproducibility

- CPU and GPU results are 3 s and 60 s runs, respectively, from a near-cold
  start. Longer runs hit the 65 °C thermal table.
- The stock CPU comparison is a frequency cap on the same image, not a separate
  stock boot image.
- The 20 s GPU A/B is the cleanest comparison; 60 s numbers are stability data,
  not a like-for-like baseline.
- Android background activity gives a 2–4 % run-to-run spread. Three clean GPU
  runs and multiple CPU runs are reported so the spread is visible.

## Privacy

The logs contain only frequencies, voltages, temperatures, bandwidth, frame
counts, and process statistics. They contain no device serial numbers, IMEI,
MAC addresses, IP addresses, usernames, Wi-Fi names, or host paths.
