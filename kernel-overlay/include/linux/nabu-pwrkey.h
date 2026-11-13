/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * nabu: interface between the PMIC power key driver
 * (drivers/input/misc/pm8941-pwrkey.c) and the platform suspend driver
 * (drivers/soc/qcom/nabu-suspend.c).
 *
 * The suspend path is debugged through the PMIC SDAM black box, so these
 * helpers stay available to the suspend code; the driver keeps the only
 * reference to the power key device.
 *
 * Details: nabu-linux-overlay/docs/port-6.18.md
 */
#ifndef _LINUX_NABU_PWRKEY_H
#define _LINUX_NABU_PWRKEY_H

#include <linux/types.h>

/* record a suspend phase marker in the PMIC SDAM (survives a reset) */
void nabu_suspend_mark(u8 v);

/* 32-bit value into the SDAM scratch area (diagnostics) */
void nabu_suspend_word(u8 off, u32 v);

/* the press that ends a platform mem suspend must reach userspace */
void nabu_pwrkey_allow_wake(void);

/* raw PON access, for the suspend path and the SDAM black box */
unsigned int nabu_pwrkey_irq(void);
void nabu_pwrkey_irq_off(void);
void nabu_pwrkey_irq_on(void);
void nabu_pwrkey_drain(void);
void nabu_pwrkey_rearm(void);
bool nabu_pwrkey_pressed(void);
u32 nabu_pwrkey_en(void);
u32 nabu_pwrkey_latched(void);

#endif /* _LINUX_NABU_PWRKEY_H */
