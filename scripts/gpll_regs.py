#!/usr/bin/env python3
"""Reproduce the SC9863A GPLL register calculation.

This mirrors the vendor kernel's drivers/clk/sprd/pll.c
(_sprd_pll_set_rate) for the gpll clock:

    SPRD_PLL_WITH_ITABLE_K_FVCO(gpll_clk, "gpll", "gpll-gate", 0x38,
                                3, ftable, f_gpll, 240,
                                1000, 1000, 1, 400000000);

The table is informational: the actual programming is done by the kernel when
it applies assigned-clock-rates. Use this to understand or audit a target.

Examples:
    python3 gpll_regs.py 550 650 700
    python3 gpll_regs.py --check
"""

from __future__ import annotations

import argparse
import sys

REFIN = 26_000_000
FVCO_THRESHOLD = 400_000_000
KINT_BITS = 23
KINT_SCALE = 1 << KINT_BITS
IBIAS_TABLE = [
    (1_000_000_000, 0),
    (1_200_000_000, 1),
    (1_400_000_000, 2),
    (1_600_000_000, 3),
]


def round_closest(num: int, den: int) -> int:
    """DIV_ROUND_CLOSEST_ULL for non-negative integers."""
    return (num + den // 2) // den


def ibias_for(fvco: int) -> int:
    for limit, value in IBIAS_TABLE:
        if fvco <= limit:
            return value
    raise ValueError(f"target {fvco} Hz is above the 1.6 GHz ibias table")


def gpll_regs(rate_hz: int) -> dict:
    if rate_hz <= 0:
        raise ValueError("rate must be positive")

    fvco = rate_hz
    postdiv = 0
    if fvco <= FVCO_THRESHOLD:
        postdiv = 1
        fvco *= 2

    ibias = ibias_for(fvco)
    nint = fvco // REFIN
    rem = fvco % REFIN

    if rem == 0:
        mode = "integer"
        kint = None
        # The driver writes div_s=0, sdm_en=0, ibias, N; mod_en is preserved.
        reg0 = ((nint << 8) | (ibias << 6)) & 0xFFFFFFFF
        reg1 = None
        reg2 = 0x10000 if postdiv else 0
        recalc = (REFIN // 2 if postdiv else REFIN) * nint
    else:
        mode = "sdm"
        kint = round_closest(rem * KINT_SCALE, REFIN)
        if kint >= KINT_SCALE:
            raise ValueError("kint overflow")
        # div_s=1, sdm_en=1, ibias; N is not written in SDM mode.
        reg0 = (0x5 | (ibias << 6)) & 0xFFFFFFFF
        reg1 = ((nint << 23) | kint) & 0xFFFFFFFF
        reg2 = 0x10000 if postdiv else 0
        refin_eff = REFIN // 2 if postdiv else REFIN
        recalc = refin_eff * nint + round_closest(refin_eff * kint, KINT_SCALE)

    return {
        "target": rate_hz,
        "mode": mode,
        "postdiv": postdiv,
        "n": nint,
        "k": kint,
        "ibias": ibias,
        "reg0": reg0,
        "reg1": reg1,
        "reg2": reg2,
        "recalc": recalc,
    }


def fmt_reg(value: int | None) -> str:
    return "not written" if value is None else f"0x{value:08x}"


def print_row(row: dict) -> None:
    k = "—" if row["k"] is None else f"0x{row['k']:06x}"
    print(
        f"{row['target'] / 1e6:>8.2f} MHz  "
        f"{row['mode']:>7s}  "
        f"N={row['n']:>3d}  K={k:>8s}  "
        f"ibias={row['ibias']}  postdiv={row['postdiv']}  "
        f"reg0={fmt_reg(row['reg0'])}  "
        f"reg1={fmt_reg(row['reg1'])}  "
        f"reg2={fmt_reg(row['reg2'])}  "
        f"recalc={row['recalc'] / 1e6:.3f} MHz"
    )


def parse_target(text: str) -> int:
    value = int(text, 0)
    if value <= 10_000:
        value *= 1_000_000
    return value


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Compute SC9863A GPLL register values.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("targets", nargs="*", default=["550", "650", "700"],
                    help="targets in MHz (or Hz with a suffix/0x prefix)")
    ap.add_argument("--check", action="store_true",
                    help="verify the known 550/650/700 rows and exit")
    args = ap.parse_args()

    if args.check:
        expected = {
            550_000_000: ("sdm", 21, 0x13B13B, 0x00000005, 0x0A93B13B, 0x00000000),
            650_000_000: ("integer", 25, None, 0x00001900, None, 0x00000000),
            700_000_000: ("sdm", 26, 0x762762, 0x00000005, 0x0D762762, 0x00000000),
        }
        ok = True
        for hz, exp in expected.items():
            row = gpll_regs(hz)
            got = (row["mode"], row["n"], row["k"], row["reg0"],
                   row["reg1"], row["reg2"])
            match = got == exp
            exact = abs(row["recalc"] - hz) <= 2
            print_row(row)
            print(f"  expected: {exp}")
            print(f"  result  : {'OK' if match and exact else 'MISMATCH'}")
            ok = ok and match and exact
        return 0 if ok else 1

    for text in args.targets:
        try:
            row = gpll_regs(parse_target(text))
        except ValueError as exc:
            print(f"{text}: {exc}", file=sys.stderr)
            return 1
        print_row(row)
        if row["recalc"] != row["target"]:
            print(
                f"  warning: recalculated {row['recalc']} Hz != target "
                f"{row['target']} Hz",
                file=sys.stderr,
            )
    return 0


if __name__ == "__main__":
    sys.exit(main())
