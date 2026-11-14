# GPLL registers — SC9863A

The GPU core and GPU SOC clocks are muxed from several TWPLL taps and from
`gpll`. At the top OPP the mux selects parent index 9, which is `gpll`. The
vendor `pvrsrvkm` driver can reparent the mux but does not program the PLL rate.
This project programs GPLL through `assigned-clock-rates` on the GPU node.

This document records the PLL math so the values can be audited and extended.

---

## 1. Driver definition

From `drivers/clk/sprd/sc9863a-clk.c`:

```c
static const struct clk_bit_field f_gpll[PLL_FACT_MAX] = {
	{ .shift = 95,	.width = 1 },	/* lock_done	*/
	{ .shift = 0,	.width = 1 },	/* div_s	*/
	{ .shift = 1,	.width = 1 },	/* mod_en	*/
	{ .shift = 2,	.width = 1 },	/* sdm_en	*/
	{ .shift = 0,	.width = 0 },	/* refin	*/
	{ .shift = 6,	.width = 2 },	/* ibias	*/
	{ .shift = 8,	.width = 11 },	/* n		*/
	{ .shift = 55,	.width = 7 },	/* nint		*/
	{ .shift = 32,	.width = 23},	/* kint		*/
	{ .shift = 0,	.width = 0 },	/* prediv	*/
	{ .shift = 80,	.width = 1 },	/* postdiv	*/
};

static SPRD_PLL_WITH_ITABLE_K_FVCO(gpll_clk, "gpll", "gpll-gate", 0x38,
				   3, ftable, f_gpll, 240,
				   1000, 1000, 1, 400000000);
```

| Parameter | Value |
|---|---|
| Register base | `0x40353000` (`anlg_phy_g2_regs`) |
| Register offset | `0x38` |
| Registers | 3 × 32-bit |
| Reference | 26 MHz |
| `udelay` | 240 µs |
| `k1`, `k2` | 1000, 1000 |
| `fflag` | 1 |
| FVCO threshold | 400 MHz |
| Clock ID | `CLK_GPLL` = 23 (`0x17`) |

`ftable` selects the PLL bias current:

| FVCO | `ibias` |
|---:|---:|
| ≤ 1.0 GHz | 0 |
| ≤ 1.2 GHz | 1 |
| ≤ 1.4 GHz | 2 |
| ≤ 1.6 GHz | 3 |

Both 650 MHz and 700 MHz are below 1.0 GHz, so `ibias = 0`.

---

## 2. Bit fields

| Register | Bits | Field |
|---|---|---|
| 0 | 0 | `div_s` |
| 0 | 1 | `mod_en` (preserved by the write mask) |
| 0 | 2 | `sdm_en` |
| 0 | 6–7 | `ibias` |
| 0 | 8–18 | `N` |
| 1 | 0–22 | `kint` |
| 1 | 23–29 | `nint` |
| 2 | 16 | `postdiv` |

For `fvco > 400 MHz`, `postdiv = 0` and `fvco = rate`. For lower rates the
driver doubles `fvco` and sets `postdiv = 1`.

Integer mode (`rate % 26 MHz == 0`):

```text
div_s  = 0
sdm_en = 0
N      = rate / 26 MHz
reg0   = (N << 8) | (ibias << 6)
reg1   is not written
reg2   = 0
```

SDM mode:

```text
div_s  = 1
sdm_en = 1
nint   = floor(rate / 26 MHz)
rem    = rate % 26 MHz
kint   = round(rem * 2^23 / 26 MHz)
reg0   = 0x5 | (ibias << 6)
reg1   = (nint << 23) | kint
reg2   = 0
```

The driver write-checks the masked register values and waits 240 µs. It does
**not** poll `lock_done`. A clean write is not proof that the PLL locked; the
stress test is the practical lock test.

---

## 3. Calculated values

| Target | Mode | N | K (23-bit) | `ibias` | reg0 | reg1 | reg2 | Recalc |
|---|---:|---:|---:|---:|---|---|---|---:|
| 550 MHz (stock) | SDM | 21 | `0x13B13B` | 0 | `0x00000005` | `0x0A93B13B` | `0x00000000` | 550.000 MHz |
| **650 MHz** | **integer** | **25** | — | **0** | **`0x00001900`** | **not written** | **`0x00000000`** | **650.000 MHz** |
| 700 MHz (preview) | SDM | 26 | `0x762762` | 0 | `0x00000005` | `0x0D762762` | `0x00000000` | 699.999999 MHz |

The 700 MHz result is one hertz below the target because 24/26 cannot be
represented exactly with 23 fractional bits. The error is far below the
reference tolerance and is not a practical concern.

The helper script reproduces the table and checks the known rows:

```sh
python3 scripts/gpll_regs.py 550 650 700
python3 scripts/gpll_regs.py --check
```

---

## 4. Why 650 MHz is the clean step

650 MHz is exactly 25 × 26 MHz. It uses integer mode: no SDM, no fractional
divider, no `reg1` write, and no sigma-delta noise. That gives the best timing
margin at the fixed 900 mV `vddcore`.

550 MHz and 700 MHz both use SDM. 700 MHz is only 7.7 % above 650 MHz but moves
from an exact integer divider to a fractional one, at the same voltage. That is
why 700 MHz remains a preview: there is no voltage margin to recover a weak
sample, and the fractional mode adds jitter that cannot be characterized from
userspace on this kernel.

---

## 5. What is not observable

`CONFIG_DEBUG_FS` is disabled, `/dev/mem` is unavailable, and
`kptr_restrict=2`. The kernel does not expose a readable GPLL rate, and
`/proc/pvr/...` reports the OPP label, not a measured PLL output.

Validation is therefore:

1. `assigned-clock-rates` is present on the GPU node and the kernel applies it
   without a `clk_set_rate` error in `dmesg`;
2. the GPU OPP table advertises 650 MHz;
3. the stress workload is faster than the 550 MHz baseline by a plausible
   fraction of the clock ratio;
4. `trans_stat` shows dwell time in the 650 MHz state under load;
5. no GPU error counters increase.

See `docs/GPU-OVERCLOCK.md` for the measured results and the caveats.
