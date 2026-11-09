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
 * from the AOSS state on this platform: the broadcast timer has no awake CPU
 * to run on and the PMIC/PDC interrupt does not get through.  Do what the
 * vendor does and enter the same PSCI state from a platform suspend op.
 *
 * Details and measurements: nabu-linux-overlay/docs/port-6.18.md
 */
#include <linux/cpu_pm.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/psci.h>
#include <linux/suspend.h>

#define NABU_PSCI_SUSPEND_STATE	0x4100c244

/* nabu: black-box markers in PM8150 SDAM, see pm8941-pwrkey.c */
extern void nabu_suspend_mark(u8 v);

static int nabu_suspend_enter(suspend_state_t state)
{
	int ret;

	if (!psci_ops.cpu_suspend)
		return -EOPNOTSUPP;

	/*
	 * Announce that this CPU is going down.  With the secondary CPUs
	 * already offlined by the PM core this makes the RPMh RSC driver
	 * flush its cached sleep/wake votes (rpmh_flush()), which is what the
	 * vendor's system_pm driver does before entering the state.
	 */
	nabu_suspend_mark(0x10);

	ret = cpu_pm_enter();
	if (ret)
		return ret;

	nabu_suspend_mark(0x11);
	pr_info("nabu: entering PSCI state %#x\n", NABU_PSCI_SUSPEND_STATE);
	psci_cpu_suspend_enter(NABU_PSCI_SUSPEND_STATE);
	nabu_suspend_mark(0x12);

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
