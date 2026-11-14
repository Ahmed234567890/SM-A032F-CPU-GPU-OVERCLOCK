# RAM / DDR results

- **Device:** Samsung SM-A032F, Unisoc SC9863A, 2 GB LPDDR3
- **DDR top OPP:** 933 MHz (LPDDR3-1866)
- **Overclock:** **none** — this is a characterization test, not a modification

---

## 1. Why this test exists

The CPU and GPU overclocks increase demand on the memory path. Before claiming
the system is stable, it is worth knowing:

- whether the DDR can reach its top OPP under load;
- whether the AP or CP vote is capping it;
- what the real read/write/copy bandwidth is;
- whether the memory path shows errors or thermal problems.

The result is also the reason the guide does **not** overclock the DDR.

---

## 2. Tool and method

`scripts/ram_stress.c`:

- 8 threads, one buffer per thread, pinned to CPUs 0–7;
- 16 MiB per thread = 128 MiB total;
- three phases: `write` (`memset`), `read` (64-bit sum), `copy` (`memcpy`
  between halves);
- sampler every 100 ms reads:
  - `sprd-governor/ddrinfo_cur_freq`
  - `ddrinfo_ap_freq` / `ddrinfo_cp_freq`
  - `scaling_force_ddr_freq`
  - CPU0/CPU4 `scaling_cur_freq`
  - `thermal_zone1` temperature
  - `MemAvailable`
- runs as root with nice `-20`;
- never writes DDR sysfs, never forces a frequency, never touches storage.

The real DDR clock on this kernel is
`/sys/class/devfreq/scene-frequency/sprd-governor/ddrinfo_cur_freq`.
`/sys/class/devfreq/scene-frequency/cur_freq` reads `0` and is not useful.

---

## 3. Results

| Metric | Probe run (3 s phases) | Confirmation run (2 s phases) |
|---|---:|---:|
| Write bandwidth | **6.68 GB/s** | **6.66 GB/s** |
| Read bandwidth | **5.73 GB/s** | **5.74 GB/s** |
| Copy bandwidth (R+W) | **5.84 GB/s** | **5.87 GB/s** |
| DDR min / max | 400 / **933 MHz** | 400 / **933 MHz** |
| DDR average | 840.2 MHz | 858.1 MHz |
| Samples at 933 MHz | 89 / 112 (79 %) | 56 / 67 (84 %) |
| AP max | 933 MHz | 933 MHz |
| CP max | 622 MHz | 400 MHz |
| `scaling_force_ddr_freq` | 0 | 0 |
| CPU temperature min / max | 58.7 / 80.1 °C | 62.3 / 78.3 °C |
| `MemAvailable` min | 652 MB | 654 MB |

Theoretical peak for a 32-bit LPDDR3 bus at 933 MHz:

```text
933 MHz × 2 (DDR) × 4 bytes = 7.464 GB/s
```

Measured efficiency:

| Phase | Measured | % of theoretical peak |
|---|---:|---:|
| Write | 6.68 GB/s | 89.5 % |
| Read | 5.73 GB/s | 76.8 % |
| Copy | 5.84 GB/s | 78.2 % |

Write is close to the theoretical peak. Read and copy are lower because the
fabric, the CPU-side load/store path, and the copy loop's read+write pattern are
not perfectly efficient. These are healthy numbers for this class of SoC.

---

## 4. Interpretation

- The DDR reaches 933 MHz under load and holds it for most of the run.
- The AP votes for 933 MHz; the CP vote is lower, which is normal (the modem
  does not need the top DDR OPP).
- `scaling_force_ddr_freq` stays 0, so nothing is being forced from userspace.
- The average is below 933 MHz because the run starts and ends at 400 MHz, and
  the sampler includes idle samples.
- The CPU temperature reaches 78–80 °C because the run is sustained and the
  phone is charging. This is the RAM stress test, not a normal workload; it is
  also why the test is kept short.
- No data mismatch, panic, filesystem error, or thermal-critical event occurred.

---

## 5. Why the DDR is not overclocked

- **933 MHz is the top entry** in the platform's DDR DFS table.
- The table is owned by the **PM co-processor firmware**, not the AP kernel.
- The AP-side `sprd-governor` also hardcodes 933 MHz.
- 1000 MHz would mean LPDDR3-2000, above the platform's validated 1866 MT/s.
- LPDDR3 has **no ECC**. Running outside the timing margin can cause silent data
  corruption, not just a crash.
- `pm_sys` is a critical boot component; a bad image can prevent boot.

The theoretical gain from 933 to 1000 MHz is only about 7.2 % in peak bandwidth,
and the measured read/copy bandwidth is already fabric-limited. The risk is not
worth the return. This project leaves the DDR at 933 MHz.

---

## 6. Raw logs

| File | Content |
|---|---|
| [`logs/ram_probe.out`](logs/ram_probe.out) | 3 s phases, 112 DDR samples |
| [`logs/ram_stress_confirm.out`](logs/ram_stress_confirm.out) | 2 s phases, 67 DDR samples |

The logs contain only bandwidth, frequencies, temperatures, and memory
availability. They do not contain device identifiers or host paths.

---

## 7. Reproduction

```sh
clang -O3 -pthread -o ram_stress ram_stress.c
su -c 'nice -n -20 ./ram_stress -t 8 -s 16 -d 2 -b 1'
```

Expected range:

| Metric | Expected |
|---|---|
| DDR max | 933 MHz |
| Write | 6.6–6.7 GB/s |
| Read | 5.7 GB/s |
| Copy | 5.8–5.9 GB/s |
| Samples at 933 | 75–85 % |
| `scaling_force_ddr_freq` | 0 |

If the DDR does not reach 933 MHz, check whether another process is holding a
low DDR vote, or whether the device is thermally limited. Do not force the
frequency from userspace to make the number look better.
