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
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/irqflags.h>
#include <linux/kernel.h>
#include <linux/psci.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>
#include <clocksource/arm_arch_timer.h>

#define NABU_PSCI_SUSPEND_STATE	0x4100c244

extern void nabu_suspend_mark(u8 v);
extern void nabu_pwrkey_allow_wake(void);

#define NABU_SETTLE_MS	2000	/* longer than this: a real wakeup */
#define NABU_WINDOW_MS	1500	/* only retry inside this window */

static int nabu_suspend_enter(suspend_state_t state)
{
	u64 settle, window, start;
	int i;

	if (!psci_ops.cpu_suspend)
		return -EOPNOTSUPP;

	/*
	 * PSCI returns immediately while any interrupt is pending at the GIC,
	 * and the SPMI arbiter summary is a chained interrupt, so
	 * suspend_device_irqs() never masks or synchronises it: a single
	 * latched PMIC event (a power on key event from boot, an RTC alarm,
	 * the key release of the press that requested the suspend) keeps the
	 * line asserted and the AOSS state is never entered.
	 *
	 * Let the interrupt path run once before each attempt so the PMIC
	 * latch and the arbiter status are acknowledged the normal way (the
	 * generic chained flow, not a hand made EOI), and retry until the
	 * state really sticks.  No PMIC/SDAM access is made here: the bus is
	 * partly gated at this point and a transaction can stall.
	 */
	settle = (u64)arch_timer_get_rate() * NABU_SETTLE_MS / 1000;

	/*
	 * Acknowledge the pending event before every attempt - but only inside
	 * the settle window.  The entry transient (the release of the key that
	 * requested the suspend) latches a PMIC event that keeps the SPMI
	 * summary asserted, and PSCI refuses to sleep while an interrupt is
	 * pending.  Outside that window the interrupts are left alone, because
	 * the pwrkey handler swallows events while suspended and a real wake
	 * press would be lost (the tablet would sleep again with nothing left
	 * to wake it).
	 */
	window = (u64)arch_timer_get_rate() * NABU_WINDOW_MS / 1000;
	start = arch_timer_read_counter();

	for (i = 0; i < 40; i++) {
		u64 t;

		local_irq_enable();
		cpu_relax();
		local_irq_disable();

		t = arch_timer_read_counter();
		psci_cpu_suspend_enter(NABU_PSCI_SUSPEND_STATE);

		if (arch_timer_read_counter() - t > settle)
			break;			/* real wakeup */
		if (arch_timer_read_counter() - start > window)
			break;			/* transient never settles */
		mdelay(10);
	}

	return 0;
}

static const struct platform_suspend_ops nabu_suspend_ops = {
	.valid = suspend_valid_only_mem,
	.enter = nabu_suspend_enter,
};

/*
 * The press that ends the platform suspend must reach userspace, otherwise
 * the SoC resumes but the display stays off and the tablet looks dead.  The
 * pending key interrupt is handled right after arch_suspend_enable_irqs(),
 * i.e. before the pwrkey device resumes, so clear its "suspended" flag from
 * the syscore phase.  This only touches driver state, no PMIC access.
 */
static void nabu_syscore_resume(void *data)
{
	nabu_pwrkey_allow_wake();
}

static const struct syscore_ops nabu_syscore_ops = {
	.resume = nabu_syscore_resume,
};

static struct syscore nabu_syscore = {
	.ops = &nabu_syscore_ops,
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

	register_syscore(&nabu_syscore);
	register_pm_notifier(&nabu_pm_nb);
	suspend_set_ops(&nabu_suspend_ops);
	pr_info("nabu: platform mem suspend registered (PSCI %#x)\n",
		NABU_PSCI_SUSPEND_STATE);
	return 0;
}
device_initcall(nabu_suspend_init);
