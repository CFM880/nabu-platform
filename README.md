# nabu-platform

**English** | 中文

SM8150 platform device tree base and the Xiaomi Pad 5 (`nabu`) device tree.

This is a direct source overlay, like the other `nabu-*` repositories: kernel
files keep their original relative paths and are copied over a chosen kernel
baseline by `nabu-main`.

## Contents

```text
kernel-overlay/
├── arch/arm64/boot/dts/qcom/Makefile          # nabu dtb entry only
├── arch/arm64/boot/dts/qcom/sm8150.dtsi       # uart13, APR/slimbus, UFS GDSC, crypto
├── arch/arm64/boot/dts/qcom/pm8150.dtsi       # RTC label, SDAM
├── arch/arm64/boot/dts/qcom/sm8150-wcd9340.dtsi
├── arch/arm64/boot/dts/qcom/sm8150-xiaomi-nabu.dts
├── drivers/of/property.c                      # remote-endpoint supplier fix
└── drivers/ufs/host/ufs-qcom.c                 # Samsung UFS runtime quirks
```

`drivers/ufs/host/ufs-qcom.c` is the 6.18.59 file with a Samsung-only runtime
quirk: some Pad 5 units carry a Samsung KLUEG8UHDC UFS whose link drops at
HS-G4 and which returns silently corrupted data when the link is runtime
suspended.  The vendor is only known after the descriptors are read, so the
limits are applied in `ufs_qcom_apply_dev_quirks()` (HS-G3, no runtime PM,
200 us ref-clock gating wait); every other vendor keeps HS-G4 and runtime PM.
Re-sync this file when the kernel base changes.  Background:
`nabu-linux-overlay/docs/KNOWN-ISSUES.md` 2.7.

## Notes

- Per-module device tree fragments stay in their own repositories
  (`sm8150-xiaomi-nabu-iris.dtsi`, `...-camera.dtsi`, `...-power.dtsi`,
  `...-accelerometer-slpi-boot-only.dtsi`); `nabu-main compose` includes them
  from the product module order.
- `pm8150b.dtsi` is deliberately **not** here: its only change adds the fuel
  gauge node, so it lives in `nabu-power`.
- Non-nabu devices from the original port range (cepheus, oneplus guacamole /
  hotdogb) are intentionally dropped, including their `Makefile` entries.
