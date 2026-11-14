# Scripts

All Python tools use only the standard library. The shell tools are Bash except
`bench_cool.sh`, which runs on the phone under Android's `sh`.

---

## Boot image tools

| Script | Runs on | Purpose |
|---|---|---|
| `unpack_boot.py` | host | Parse the Android boot header, extract kernel/ramdisk/DTBO/FDT, optionally decompile with `dtc` |
| `pack_dtbo.py` | host | Build a big-endian DTBO container around a patched FDT, insert it, update `dtb_size`, preserve kernel/ramdisk/id |
| `verify_boot.py` | host | Structural checks, DTBO byte order, FDT header, optional CPU/GPU property checks, reference comparison |
| `gpll_regs.py` | host | Reproduce the SC9863A GPLL register math; `--check` validates 550/650/700 |

Typical sequence:

```sh
python3 unpack_boot.py --image boot-stock.img --outdir work
dtc -I dtb -O dts -o work/fdt.dts work/fdt.dtb
# edit work/fdt.dts
dtc -I dts -O dtb -o work/fdt-patched.dtb work/fdt.dts
python3 pack_dtbo.py --image boot-stock.img \
  --fdt work/fdt-patched.dtb --out boot-oc.img
python3 verify_boot.py --image boot-oc.img --reference boot-stock.img
```

`pack_dtbo.py` defaults match the A032FXXS9CZA2 layout. Override
`--dtbo-off`, `--dtb-size-off`, and `--clear-size` if your image differs.

---

## Flash tools

| Script | Runs on | Purpose |
|---|---|---|
| `flash_boot.sh` | host | Copy an image over SSH, verify SHA-256 on the phone, write only `/dev/block/by-name/boot`, sync, reboot |
| `rollback_boot.sh` | host | Same as `flash_boot.sh` with a louder warning |
| `bench_cool.sh` | phone | Turn the screen off and wait until `thermal_zone1` reaches a target temperature |

```sh
./flash_boot.sh --image boot-oc.img --phone "$PHONE" --port "$SSH_PORT" \
  --user "$SSH_USER" --key "$SSH_KEY"

./rollback_boot.sh --image boot-stock.img --phone "$PHONE" --port "$SSH_PORT" \
  --user "$SSH_USER" --key "$SSH_KEY"
```

Both scripts require an explicit `--phone` and `--user`; nothing is hardcoded.
They refuse to continue if the remote hash does not match. Use `--dry-run` to
inspect the commands, and `--yes` only when you are certain.

---

## Benchmark sources

| Source | Build | Purpose |
|---|---|---|
| `cpu_bench.c` | `clang -O2 -pthread -o cpu_bench cpu_bench.c` | Dependent integer chain, single or all-core, reports Miter/s, residency, voltage, temperature |
| `gpu_stress.c` | `clang -O2 -pthread -o gpu_stress gpu_stress.c -ldl` | EGL/GLES2 offscreen shader load, samples GPU/CPU frequency, temperature, `vddcore`, DDR |
| `ram_stress.c` | `clang -O3 -pthread -o ram_stress ram_stress.c` | Multithreaded memory write/read/copy plus DDR frequency sampler |
| `analyze_bench.py` | host Python | Parse `cpu_bench` output and legacy logs into a table |

The phone's userspace is 32-bit (`armeabi-v7a`), so build on the phone with
Termux's `clang`. Cross-compiling with a 64-bit toolchain produces a binary the
ROM cannot run.

`gpu_stress` binds to the vendor EGL/GLES libraries through `dlopen`. Run it from
Termux, not from a minimal chroot without the vendor graphics libraries.

---

## Safety

- `unpack_boot.py`, `pack_dtbo.py`, `verify_boot.py`, and `gpll_regs.py` are
  read-only with respect to the phone.
- `cpu_bench`, `gpu_stress`, and `ram_stress` only read sysfs/procfs and render
  to their own buffers. They do not write devfreq, regulator, or thermal sysfs.
- `flash_boot.sh` and `rollback_boot.sh` write the boot partition. That is the
  only destructive operation in this repository.
- `bench_cool.sh` sends a screen-off key event and sleeps; it does not change
  CPU/GPU state.

Never commit a boot image, kernel, vendor blob, or GPU firmware. The packers in
this directory rebuild images from a dump you provide.
