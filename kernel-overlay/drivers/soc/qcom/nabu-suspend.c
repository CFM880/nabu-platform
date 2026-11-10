// SPDX-License-Identifier: GPL-2.0-only
/*
 * nabu: platform "mem" suspend for the Xiaomi Pad 5 (SM8150).
 *
 * The vendor kernel does not use s2idle on this SoC: its LPM framework
 * offlines the secondary CPUs (done by the PM core for any platform suspend
 * state), lets the last CPU flush the RPMh sleep votes and program the PDC
 * wakeup timer, and then enters the PSCI AOSS sleep state
 * (0x4100c244 = affinity 1, cluster mode 0xC24 "LLCC off / AOSS sleep",
 * CPU mode 0x4 "rail power collapse").
 *
 * Mainline only registers s2idle for this SoC, and s2idle cannot be woken
 * from the AOSS state on this platform.  Do what the vendor does and enter
 * the same PSCI state from a platform suspend op.
 *
 * The PMIC asserts a false ~180ms power-key press right after the state is
 * entered (measured), and every edge returns from the PSCI.  Re-enter the
 * state a few times inside .enter() until the line has settled; a real key
 * press later comes back after the settle window and resumes normally.
 *
 * Details and measurements: nabu-linux-overlay/docs/port-6.18.md
 */
#include <linux/cpu_pm.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <clocksource/arm_arch_timer.h>
#include <linux/psci.h>
#include <linux/suspend.h>

#define NABU_PSCI_SUSPEND_STATE	0x4100c244
#define NABU_SETTLE_MS		2000

/* nabu: black-box markers in PM8150 SDAM, see pm8941-pwrkey.c */
extern void nabu_suspend_mark(u8 v);
extern void nabu_pwrkey_rearm(void);

static int nabu_suspend_enter(suspend_state_t state)
{
	u64 start, settle;
	int ret, i;

	if (!psci_ops.cpu_suspend)
		return -EOPNOTSUPP;

	nabu_suspend_mark(0x10);

	/*
	 * Announce that this CPU is going down.  With the secondary CPUs
	 * already offlined by the PM core this makes the RPMh RSC driver
	 * flush its cached sleep/wake votes, which is what the vendor's
	 * system_pm driver does before entering the state.
	 */
	ret = cpu_pm_enter();
	if (ret)
		return ret;

	/* the vendor cluster_notify() does this for is_reset levels */
	cpu_cluster_pm_enter();

	/*
	 * Timekeeping is suspended while .enter() runs, so ktime_get() is not
	 * usable here; use the raw arch counter instead.
	 */
	settle = (u64)arch_timer_get_rate() * NABU_SETTLE_MS / 1000;
	start = arch_timer_read_counter();

	for (i = 0; i < 60; i++) {
		/*
		 * The IRQ core masks the interrupt (the PMIC EN and the PDC
		 * pin enable) when it reports the false wake; restore the IRQ
		 * state like the resume path would, then re-arm the wakeup.
		 * Without this the key can never wake us again (the charger
		 * IRQ, which was never disabled, still can).
		 */
		resume_device_irqs();
		nabu_pwrkey_rearm();
		nabu_suspend_mark(0x11);

		psci_cpu_suspend_enter(NABU_PSCI_SUSPEND_STATE);

		nabu_suspend_mark(0x12);
		if (arch_timer_read_counter() - start > settle)
			break;
	}

	cpu_cluster_pm_exit();
	cpu_pm_exit();

	return 0;
}

static const struct platform_suspend_ops nabu_suspend_ops = {
	.valid = suspend_valid_only_mem,
	.enter = nabu_suspend_enter,
};

static int __init nabu_suspend_init(void)
{
	if (!psci_ops.cpu_suspend)
		return -ENODEV;

	suspend_set_ops(&nabu_suspend_ops);
	pr_info("nabu: platform mem suspend registered (PSCI %#x)\n",
		NABU_PSCI_SUSPEND_STATE);
	return 0;
}
device_initcall(nabu_suspend_init);
