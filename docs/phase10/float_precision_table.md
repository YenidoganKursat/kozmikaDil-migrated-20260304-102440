# Float Precision Conformance Table (Operator-Level)

Date: 2026-02-18

Validation command:
- `python3 bench/scripts/primitives/check_float_ops_stepwise_bigdecimal.py --loops 100000 --skip-kozmika --python-crosscheck --python-loops 100000`

Method:
- Random inputs are generated in binary-exact form (`((seed % 4097) - 2048) / 256.0`) to remove decimal parsing noise.
- Inputs are first quantized to each primitive type.
- Reference is then computed from these typed inputs with high precision (`BigDecimal` / Python `decimal`) and rounded back to the same primitive.
- Reported errors are against this typed reference (operator conformance check).

## Theoretical Limits and Measured Conformance (`+,-,*,/`)

| Type | Epsilon @1 (theory) | Min normal (theory) | Min subnormal (theory) | Max abs error (`+,-,*,/`) | Max abs / epsilon@1 (`+,-,*,/`) |
|---|---:|---:|---:|---:|---:|
| `f8` (e4m3 model) | `1.250e-1` | `1.563e-2` | `1.953e-3` | `0.000e+0` | `0.000e+0` |
| `f16` | `9.766e-4` | `6.104e-5` | `5.960e-8` | `0.000e+0` | `0.000e+0` |
| `bf16` | `7.812e-3` | `1.175e-38` | `9.184e-41` | `0.000e+0` | `0.000e+0` |
| `f32` | `1.192e-7` | `1.175e-38` | `1.401e-45` | `0.000e+0` | `0.000e+0` |
| `f64` | `2.220e-16` | `2.225e-308` | `4.941e-324` | `0.000e+0` | `0.000e+0` |
| `f128` (target) | `1.926e-34` | `3.362e-4932` | `6.475e-4966` | `0.000e+0` | `0.000e+0` |
| `f256` (target) | `9.056e-72` | `2.482e-78913` | `2.248e-78984` | `0.000e+0` | `0.000e+0` |
| `f512` (target) | `7.821e-149` | `2.482e-78913` | `1.941e-79061` | `0.000e+0` | `0.000e+0` |

## Modulo Note (`%`)

`%` is numerically discontinuous around quotient boundaries and not a good "near-epsilon" metric.
For `f64`, current run gives:
- max abs error: `1.563499e-16`
- max abs / epsilon@1: `7.041373e-01`

For `f128/f256/f512`, `%` vs theoretical epsilon@1 is very large because runtime is still
backed by `double/long double` behavior on this platform.

## Important Limit

This table proves operator-path conformance to the current typed model.
It does **not** prove true hardware/software `f128/f256/f512` precision capability.
True `f128+` precision still requires a dedicated multiprecision backend (MPFR/SoftFloat-like path).
