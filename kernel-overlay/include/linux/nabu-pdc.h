/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * nabu: PDC (Power Domain Controller) helpers exported for the suspend
 * diagnostics.
 *
 * The PDC configuration is lost when the always-on domain collapses, and the
 * wake source wiring is the delicate part of platform mem suspend; these
 * accessors let the suspend code (and a debug driver) inspect and re-program
 * single pins.  See nabu-linux-overlay/docs/port-6.18.md.
 */
#ifndef _LINUX_NABU_PDC_H
#define _LINUX_NABU_PDC_H

#include <linux/types.h>

/* re-write the saved per-pin configuration and the enable banks */
void nabu_pdc_restore(void);

/* pin accessors, by absolute PDC pin number */
void nabu_pdc_enable_pin(int pin);
unsigned int nabu_pdc_pin_irq(int pin);
u32 nabu_pdc_pin_cfg(int pin);
u32 nabu_pdc_pin_enabled(int pin);

#endif /* _LINUX_NABU_PDC_H */
