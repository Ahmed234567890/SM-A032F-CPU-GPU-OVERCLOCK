# Troubleshooting

Every failure mode below was either hit during this project or is a direct
consequence of the mechanisms involved. When in doubt, roll back first and
diagnose from a booted phone.

---

## 0. First rule: roll back

If the phone does not boot, does not display, or shows GPU corruption:

```sh
# From a booted Android / adb / SSH shell.
su -c 'dd if=/sdcard/boot-stock.img of=/dev/block/by-name/boot bs=1M conv=fsync'
sync
reboot
```

If Android does not start, use download mode/Odin with the stock boot image.
Do not keep flashing experimental images while the device is unstable.

---

## 1. Boot failures

### 1.1 Black screen or boot loop after flashing

| Check | What to do |
|---|---|
| Did the image hash match on the phone? | Re-copy and re-check `sha256sum` |
| Did you write the right partition? | It must be `/dev/block/by-name/boot` |
| Is the DTBO magic big-endian? | First four bytes must be `d7 b7 ab 1e` |
| Does `dtb_size` match the container? | `verify_boot.py` checks this |
| Are kernel and ramdisk unchanged? | Compare against the source image |
| Is the FDT `totalsize` correct? | `verify_boot.py` checks the FDT header |

Recovery order:

1. Force a reboot (hold Power ~10 s).
2. If Android still does not boot, connect USB and try `adb devices`.
3. If `adb` works, `dd` the stock/v4 image back.
4. If `adb` does not work, boot download mode and use Odin.
5. If download mode does not work, stop and seek service help. Do not guess.

### 1.2 Phone boots but Wi-Fi/Bluetooth/camera is broken

The boot image contains only kernel + ramdisk + DTBO. A broken peripheral
usually means the DTBO edit touched a node it should not have, or the DTBO was
truncated.

- Reflash the stock boot image.
- Diff your FDT against the stock FDT:
  ```sh
  fdtdump stock.dtb > stock.txt
  fdtdump patched.dtb > patched.txt
  diff -u stock.txt patched.txt | less
  ```
- Only the intended OPP properties should differ.

### 1.3 Phone boots but the overclock is absent

See the CPU and GPU sections below. The most common cause is a patch applied to
the wrong table or the wrong node.

---

## 2. DTBO and boot-image problems

### 2.1 `DTBO magic is little-endian`

Symptom:

```text
first four bytes: 1e ab b7 d7
```

Cause: the container was written with the magic as a little-endian integer.

Fix: repack with `scripts/pack_dtbo.py`, or use Android's `mkdtboimg.py`, which
writes the standard big-endian header. Verify:

```sh
xxd -s 0x13be000 -l 4 boot.img
# expected: d7b7ab1e
```

### 2.2 `dtb_size` mismatch

Symptom: the bootloader ignores the new tree, or the kernel sees a truncated
FDT.

`dtb_size` at offset `0x670` must equal the **container** size (header +
entry + FDT), not the FDT size:

```text
container = 0x40 + fdt_size
dtb_size  = container
```

For a 650 MHz FDT of `0x15e8f` bytes:

```text
container = 0x40 + 0x15e8f = 0x15ecf
dtb_size  = 0x15ecf
```

`verify_boot.py` prints both values.

### 2.3 `fdtput` fails with `FDT_ERR_NOSPACE`

The live FDT may not have enough spare space for new properties. Options:

1. Decompile to DTS, add the properties, and recompile with `dtc` (the new blob
   can grow).
2. Use a DTB with more padding.
3. Remove an unused property only if you are certain it is safe.

Do not truncate an OPP table to make it fit.

### 2.4 `fdtput` succeeds but the property is missing after repack

- Confirm the node path. The GPU node is `/soc/mm/gpu@60000000` in the
  decompiled tree.
- Confirm the property name, including the comma:
  `sprd,dvfs-lists`, not `sprd-dvfs-lists`.
- Dump the packed FDT:
  ```sh
  fdtget -t x work/fdt.dtb /soc/mm/gpu@60000000 assigned-clock-rates
  ```
- If you edited a DTS and recompiled, make sure the packer used the new DTB,
  not the old one.

### 2.5 `dtc` warnings

Warnings such as `unit_address_vs_reg`, `simple_bus_reg`, or
`avoid_unnecessary_addr_size` are common on vendor trees and are not fatal. A
**syntax error** or a missing `};` is fatal. Always check the exit code:

```sh
dtc -I dts -O dtb -o out.dtb in.dts && echo OK
```

### 2.6 The image is larger than the partition

`dd` reports `No space left on device`.

- The boot partition is 64 MiB on this device. The image is exactly 64 MiB.
- If you appended data or used a larger page size, you are writing too much.
- Do not truncate the image mid-DTBO. Repack correctly.

---

## 3. CPU problems

### 3.1 New frequency does not appear

Check:

```sh
su -c 'cat /sys/devices/system/cpu/cpufreq/policy4/cpuinfo_max_freq'
su -c 'cat /sys/devices/system/cpu/cpufreq/policy4/scaling_available_frequencies'
su -c 'dmesg | grep -iE "cpufreq|opp|bin" | tail -40'
```

Likely causes:

| Cause | Fix |
|---|---|
| Patched only `operating-points` | Patch all four bin tables |
| Wrong cluster node | `cpufreq-clus1` is big, `cpufreq-clus0` is little |
| Wrong hex value | 2.0 GHz = `0x1e8480`, not `0x1e8480` typed as decimal |
| DTBO ignored | Check magic and `dtb_size` |
| Old boot image still active | Check the boot partition hash |

### 3.2 Frequency appears but is never used

| Cause | Fix |
|---|---|
| Vendor power HAL cap | Use `performance` governor + explicit `scaling_max_freq` for the test |
| Thermal fallback at 65 °C | Cool the device; accept fallback under sustained load |
| `scaling_max_freq` below the new OPP | Write the max explicitly |
| Governor choices | `schedutil` may not pick the top OPP for a light load |

Test with a deliberate load:

```sh
su -c 'echo performance > /sys/devices/system/cpu/cpufreq/policy4/scaling_governor'
su -c 'echo 2000000 > /sys/devices/system/cpu/cpufreq/policy4/scaling_max_freq'
su -c 'taskset -c 4 sh -c "yes > /dev/null & sleep 3; kill %1"'
```

Then read `time_in_state` or `scaling_cur_freq`.

### 3.3 Little cluster stays at 1.2 GHz (or voltage is 1.1 V at 1.6 GHz)

The stock little tables top out at **1.2 GHz**. 1.6 GHz only exists after the
patch adds it. If `policy0/cpuinfo_max_freq` is still `1200000`:

1. confirm all four little tables (`operating-points`, `-1`, `-2`, `-3`) were
   patched, not just the base table;
2. confirm the DTBO was accepted (magic and `dtb_size`);
3. confirm the boot partition hash matches the image you built.

If `policy0` advertises 1.6 GHz but the regulator reads about 1.1 V while the
cluster is at 1.2 GHz, the cluster is in the 65 °C thermal table or the
frequency write did not take effect. Check the temperature and the governor
before editing the table again.

### 3.4 The 2.1 GHz image does not boot

The historical 2.1 GHz image used a **byte-swapped DTBO header**. The
bootloader likely rejected the tree, so the 2.1 GHz kernel/DTB was never
exercised.

If you retry 2.1 GHz:

1. Rebuild the container with `scripts/pack_dtbo.py`.
2. Verify the first four bytes are `d7 b7 ab 1e`.
3. Verify `dtb_size` and the FDT `totalsize`.
4. Use 1.30 V (`0x13d620`) as a starting point.
5. Run the full test sequence; do not assume it works because 2.0 GHz does.

### 3.5 Little cluster falls to 1.2 GHz during a run

This is the **65 °C thermal table**, not a bug. The driver switches to
`operating-points-*-65`, which caps the little cluster at 1.2 GHz and the big
cluster at 1.6 GHz.

Options:

- cool the device to ≤ 45 °C before the run;
- shorten the run to 3 s;
- accept the fallback for sustained workloads.

Do not modify the `-65` tables to "fix" this. They are the safety net.

### 3.6 Under-voltage or regulator fault

Symptoms in `dmesg`:

```text
voltage drop
regulator ... failed
```

Roll back. The OPP voltage is too low for your sample. Try a lower frequency or
a higher voltage only after a controlled re-test.

---

## 4. GPU problems

### 4.1 `max_freq=650000000` but fps is unchanged

This is the signature of "OPP label changed, GPLL did not".

Checklist:

1. Is `assigned-clock-rates` present on the **GPU node**?
   ```sh
   fdtget -t x work/fdt.dtb /soc/mm/gpu@60000000 assigned-clocks
   fdtget -t x work/fdt.dtb /soc/mm/gpu@60000000 assigned-clock-rates
   ```
   Expected: `6 17` and `26be3680`.
2. Is the phandle really GPLL? `0x06` in the live tree, `#clock-cells = 1`.
3. Is the clock ID `0x17` (23)?
4. Did the kernel log a `clk_set_rate` failure?
   ```sh
   su -c 'dmesg | grep -iE "gpll|clk|assigned" | tail -60'
   ```
5. Did the bootloader accept the DTBO? Check the magic and `dtb_size`.
6. Does `trans_stat` show 650 MHz dwell time? If not, the OPP is not being
   selected (scene mapping/governor), even if `max_freq` says 650.

If GPLL did not move, the GPU still runs at the old rate. The kernel may log
the rate-write failure but continue to probe; this is not fatal, just no gain.

### 4.2 `assigned-clock-rates` is ignored

Possible causes:

| Cause | Fix |
|---|---|
| Property on the wrong node | It must be on `/soc/mm/gpu@60000000` |
| Wrong phandle | Use the GPLL provider, not `gpll-gate` |
| Wrong clock ID | `CLK_GPLL` = 23 (`0x17`) |
| Property name typo | `assigned-clock-rates`, plural |
| Old DTBO active | Check the boot partition hash and `dtb_size` |
| Rate write rejected | Check `dmesg`; try 650 MHz again from a clean boot |

`of_clk_set_defaults()` is called by `platform_drv_probe()` before the driver's
probe. If the GPU is not a platform device on your kernel, this mechanism does
not apply. On this vendor kernel it is.

### 4.3 `max_freq` is still 550000000

- The OPP property was not replaced. `fdtput` may have failed silently; check
  the exit code and dump the property.
- The packer used the old FDT. Compare the FDT hash inside the boot image with
  the hash of your patched DTB.
- The bootloader ignored the DTBO. Check the magic.
- You flashed a different slot/partition. This device uses
  `/dev/block/by-name/boot`; verify with `ls -l /dev/block/by-name/`.

### 4.4 GPU hangs or renders corruption

This is a timing failure at 900 mV. Roll back immediately.

- Do not "try again" repeatedly; a hanging GPU can corrupt data.
- Reboot after the hang.
- If the phone is unstable, reflash the stock/v4 boot image.
- 700 MHz is even less likely to be stable on the same sample.

### 4.5 GPU temperature above 70 °C

Abort the run, remove the charger if it is contributing heat, and cool the
device. The 650 MHz runs in this project peaked at 59.9 °C; a much higher
reading suggests a different ambient, a case, or a workload that keeps the GPU
at 650 MHz longer.

### 4.6 `cat /sys/class/devfreq/...` permission denied

The devfreq sysfs is root-only on this ROM. Use:

```sh
su -c 'cat /sys/class/devfreq/60000000.gpu/max_freq'
```

Do not set SELinux to permissive to work around this.

### 4.7 No debugfs / no `/dev/mem` / no direct GPLL read

This is expected:

- `CONFIG_DEBUG_FS` is not enabled;
- `/dev/mem`, `/dev/kmem`, and `/proc/kcore` are unavailable;
- `kptr_restrict=2`.

The only practical validation is the OPP label, the fps delta, and the
`trans_stat` dwell time. Do not chase a direct clock read on this kernel.

### 4.8 `APM Event Count` is large

`APM` is not an error counter. It increments with normal GPU activity. The
failure counters are `HWR`, `CRR`, `SLR`, `WGP`, `TRP`, and `FWF`.

### 4.9 The GPU is at 650 MHz but the game is not faster

The synthetic stress is ALU-heavy; games are often memory-, bandwidth-, or
CPU-limited. A 5–15 % gain in GPU-limited scenes is realistic. If the game is
CPU-bound, the GPU overclock will not help.

---

## 5. Benchmark problems

### 5.1 The CPU benchmark reports stock numbers

- The binary needs root to set `performance` and the frequency caps.
- Without root, the sysfs writes fail silently and `schedutil` picks
  frequencies.
- Check the `freq_cap` line in the output; it should show the requested caps.

### 5.2 The GPU stress binary fails to start

| Error | Fix |
|---|---|
| `dlopen EGL/GLES failed` | Run from Termux with the vendor GL libraries available; do not run in a minimal chroot |
| `eglChooseConfig failed` | Try a smaller pbuffer (`-s 512`) |
| `shader compile failed` | The GPU/driver does not accept the shader; report the log |
| `Permission denied` on sysfs | Run through `su -c` |

### 5.3 Results are noisy

- Cool the device.
- Turn the screen off.
- Stop background apps.
- Use the same duration for A/B.
- Run three times and report the range, not a single best run.

### 5.4 The 32-bit userspace limits the benchmark

The ROM is `armeabi-v7a`. The benchmark binaries are ARMv7. A 64-bit benchmark
is not possible without a 64-bit userspace. This does not affect the kernel or
the overclock.

---

## 6. Flash and transfer problems

### 6.1 `ssh: Permission denied (publickey)`

- Use the correct user, port, and key.
- The phone's SSH server may only allow key authentication.
- Test with `ssh -v`.
- If you normally use `gh` for GitHub, that is unrelated; the phone uses its
  own SSH endpoint.

### 6.2 `su: not found` or `su -c` fails

- Root must be set up and granted to the shell/Termux app.
- Use the full path to `su` if needed.
- Run `id` and confirm `uid=0`.

### 6.3 `dd: /dev/block/by-name/boot: Permission denied`

- Run through `su -c`.
- Confirm the by-name symlink exists:
  ```sh
  su -c 'ls -l /dev/block/by-name/boot'
  ```
- Do not write to a partition you cannot identify.

### 6.4 Hash mismatch after `scp`

- Re-copy the file.
- Check free space on `/sdcard`.
- Compare `sha256sum` on both ends.
- Do not flash a mismatched image.

### 6.5 Phone reboots before the write finishes

- Keep the charger connected.
- Do not run the flash from a session that dies when the screen turns off.
- Use `conv=fsync` and `sync`.
- If the write was interrupted, reflash from download mode; do not reboot into
  a half-written boot partition.

---

## 7. Log collection

When asking for help, include:

```sh
# Device and firmware.
getprop ro.product.model
getprop ro.build.fingerprint
uname -a

# Boot image hash (from a root shell).
su -c 'dd if=/dev/block/by-name/boot of=/sdcard/boot-live.img bs=1M'
sha256sum /sdcard/boot-live.img

# CPU/GPU state.
su -c 'cat /sys/devices/system/cpu/cpufreq/policy0/cpuinfo_max_freq'
su -c 'cat /sys/devices/system/cpu/cpufreq/policy4/cpuinfo_max_freq'
su -c 'cat /sys/class/devfreq/60000000.gpu/max_freq'
su -c 'cat /sys/class/devfreq/60000000.gpu/available_frequencies'
su -c 'cat /proc/pvr/status'
su -c 'cat /sys/class/devfreq/60000000.gpu/trans_stat'

# Kernel messages.
su -c 'dmesg' | tail -300

# Benchmark output.
cat cpu_single.out cpu_all.out gpu_60s_run1.log
```

Strip your IP address, SSH port/user, serial number, IMEI, MAC address, Wi-Fi
SSID, and any personal paths before posting publicly.

---

## 8. Known limitations

1. **No direct GPLL read.** `CONFIG_DEBUG_FS` is disabled and `/dev/mem` is
   unavailable. Validation is indirect.
2. **No GPU voltage control.** `vddcore` is fixed at 900 mV and shared. There is
   no `vddgpu` rail.
3. **No documented GPLL boot setting.** The boot stages scanned in this project
   contain no exact 550/700 MHz GPLL configuration. The boot default is not
   documented.
4. **The GPU stress workload is ALU-heavy.** Real games may be more
   memory-bound.
5. **The GPU driver source is not in the vendor kernel tree.** The DVFS behavior
   was recovered from the unstripped `pvrsrvkm.ko`; the disassembly is
   consistent, but it is not a source-level guarantee.
6. **The boot-image `id` algorithm is vendor-specific.** The packers preserve
   the existing value. Raw `dd` does not validate it.
7. **The PLL driver does not poll lock.** A clean write does not prove the PLL
   locked. The stress test is the real lock test.
8. **Recovery is not automatic.** Keep a stock boot image and a download-mode
   path.

---

## 9. Diagnostic cheat sheet

```sh
# What is the live boot partition?
su -c 'sha256sum /dev/block/by-name/boot'

# What does the packed image look like?
python3 scripts/verify_boot.py --image boot-oc.img --reference boot-stock.img

# What is in the DTBO?
xxd -s 0x13be000 -l 16 boot-oc.img
fdtdump work/fdt.dtb | head -30

# CPU.
su -c 'cat /sys/devices/system/cpu/cpufreq/policy4/cpuinfo_max_freq'
su -c 'cat /sys/devices/system/cpu/cpufreq/policy4/scaling_available_frequencies'
su -c 'cat /sys/devices/system/cpu/cpu4/cpufreq/stats/time_in_state'
su -c 'cat /sys/class/regulator/regulator.1/microvolts'
su -c 'cat /sys/class/thermal/thermal_zone1/temp'

# GPU.
su -c 'cat /sys/class/devfreq/60000000.gpu/max_freq'
su -c 'cat /sys/class/devfreq/60000000.gpu/available_frequencies'
su -c 'cat /sys/class/devfreq/60000000.gpu/trans_stat'
su -c 'cat /proc/pvr/status'
su -c 'cat /proc/pvr/gpu00/debug_dump' | grep -i 'RGX DVFS'

# Kernel.
su -c 'dmesg' | grep -iE 'gpll|clk|pvr|rgx|opp|thermal' | tail -80
```
