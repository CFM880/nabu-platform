// SPDX-License-Identifier: GPL-2.0-only
/*
 * nabu: platform "mem" suspend for the Xiaomi Pad 5 (SM8150).
 *
 * The vendor kernel does not use s2idle on this SoC: its LPM framework
 * offlines the secondary CPUs (done by the PM core for any platform suspend
 * state), lets the last CPU flush the RPMh sleep votes and program the PDC
 * wakeup timer, and then enters the PSCI AOSS sleep state
 * (0x4100c244 = reset flag, affinity 1, cluster mode 0xC24 "LLCC off / AOSS
 * sleep", CPU mode 0x4 "rail power collapse").
 *
 * Mainline only registers s2idle for this SoC, and s2idle cannot be woken
 * from the AOSS state on this platform.  Do what the vendor does and enter
 * the same PSCI state from a platform suspend op.
 *
 * The key interrupt and the PDC pin that carries the SPMI arbiter summary are
 * armed by pm8941_pwrkey_suspend(), which runs during the device suspend
 * phase with a healthy SPMI bus.  This function must not touch the PMIC at
 * all: the SPMI arbiter and the PMIC are already partially gated when the
 * platform entry runs, and a stalled SPMI transaction hangs the machine with
 * interrupts disabled (measured - the hang moves to wherever a PMIC register
 * is touched from here).
 *
 * Details and measurements: nabu-linux-overlay/docs/port-6.18.md
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/psci.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>

#define NABU_PSCI_SUSPEND_STATE	0x4100c244

extern void nabu_suspend_mark(u8 v);

static int nabu_suspend_enter(suspend_state_t state)
{
	if (!psci_ops.cpu_suspend)
		return -EOPNOTSUPP;

	/*
	 * Nothing but the PSCI call, like the vendor's from_idle=false path:
	 * the key interrupt and the PDC pin are armed during the device
	 * suspend phase and the PDC configuration is restored by the syscore
	 * resume callback, because it is lost across the sleep.  Touching any
	 * of these blocks here is unsafe: the PMIC/SMPI access can stall and
	 * the PDC is behind a clock gate by now, so a write can be dropped.
	 */
	psci_cpu_suspend_enter(NABU_PSCI_SUSPEND_STATE);

	return 0;
}

static const struct platform_suspend_ops nabu_suspend_ops = {
	.valid = suspend_valid_only_mem,
	.enter = nabu_suspend_enter,
};

/* phase marker, written from the PM notifier (healthy SPMI bus) */
static int nabu_pm_notify(struct notifier_block *nb, unsigned long action,
			  void *data)
{
	if (action == PM_SUSPEND_PREPARE)
		nabu_suspend_mark(0x08);

	return NOTIFY_DONE;
}

static struct notifier_block nabu_pm_nb = {
	.notifier_call = nabu_pm_notify,
};

static int __init nabu_suspend_init(void)
{
	if (!psci_ops.cpu_suspend)
		return -ENODEV;

	register_pm_notifier(&nabu_pm_nb);
	suspend_set_ops(&nabu_suspend_ops);
	pr_info("nabu: platform mem suspend registered (PSCI %#x)\n",
		NABU_PSCI_SUSPEND_STATE);
	return 0;
}
device_initcall(nabu_suspend_init);
