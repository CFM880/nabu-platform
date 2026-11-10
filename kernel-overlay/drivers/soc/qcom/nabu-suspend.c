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
 * entered (measured).  It is a normal PMIC interrupt, so it leaves the SPMI
 * summary line to the PDC asserted; since .enter() runs with interrupts
 * disabled nothing acknowledges it and any further entry returns at once.
 * Dispatch the chained summary line by hand after such an early return to
 * clear the latch, and keep re-entering until the state actually sticks.
 * A real key press later returns after the settle window and resumes.
 *
 * Details and measurements: nabu-linux-overlay/docs/port-6.18.md
 */
#include <linux/cpu_pm.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/psci.h>
#include <linux/suspend.h>
#include <clocksource/arm_arch_timer.h>

#define NABU_PSCI_SUSPEND_STATE	0x4100c244
#define NABU_SETTLE_MS		2000
#define NABU_MAX_ATTEMPTS	4000

/* nabu: black-box markers in PM8150 SDAM, see pm8941-pwrkey.c */
extern void nabu_suspend_mark(u8 v);
extern void nabu_pwrkey_rearm(void);
extern void nabu_pwrkey_allow_wake(void);
extern unsigned int nabu_pwrkey_irq(void);
/* PDC pin that carries the SPMI arbiter summary (all PMIC interrupts) */
extern void nabu_pdc_enable_pin(int pin);
extern unsigned int nabu_pdc_pin_irq(int pin);
#define NABU_PDC_SPMI_PIN	1

static int nabu_suspend_enter(suspend_state_t state)
{
	u64 start, settle;
	unsigned int irq;
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

	/* the PMIC interrupts have to reach the PDC even though the IRQ core
	 * disabled the (non wakeup) key interrupt at suspend */
	nabu_pdc_enable_pin(NABU_PDC_SPMI_PIN);
	nabu_pwrkey_rearm();

	for (i = 0; i < NABU_MAX_ATTEMPTS; i++) {
		nabu_suspend_mark(0x11);
		psci_cpu_suspend_enter(NABU_PSCI_SUSPEND_STATE);
		nabu_suspend_mark(0x12);

		if (arch_timer_read_counter() - start > settle) {
			/* slept for longer than the window: a real wakeup */
			nabu_pwrkey_allow_wake();
			break;
		}

		/*
		 * Returned immediately, so the PMIC event that woke us is
		 * still latched and the SPMI summary line is still asserted.
		 * Run the chained handler once, exactly like the interrupt
		 * would, so the latch is acknowledged; restore the wakeup
		 * bookkeeping the pm core would otherwise do at resume.
		 */
		irq = nabu_pdc_pin_irq(NABU_PDC_SPMI_PIN);
		if (irq)
			generic_handle_irq(irq);
		rearm_wake_irq(nabu_pwrkey_irq());
		pm_wakeup_clear(0);

		/* arm the key interrupt again for the next attempt */
		nabu_pdc_enable_pin(NABU_PDC_SPMI_PIN);
		nabu_pwrkey_rearm();
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
