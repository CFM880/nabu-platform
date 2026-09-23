# Sources

## Kernel

- Baseline range: `b9d5d463c216763cec719c04536ea9e14512cad4` (Linux 6.14.11)
  → `5181e1358ddd6ea8028e841d928942373e6aebc8` (NABU baseline)
- Tree: `https://gitlab.postmarketos.org/soc/qualcomm-sm8150/linux.git`
- `nabu-platform` carries only the Xiaomi Pad 5 (nabu) changes of that range; the
  non-nabu devices (Xiaomi Mi 9 cepheus, OnePlus 7/Pro/T) are dropped.

Files are kept at their original kernel-relative paths so they can be overlaid
onto the baseline by `nabu-main apply`.
