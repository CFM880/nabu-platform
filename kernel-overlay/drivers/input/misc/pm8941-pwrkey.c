// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2010-2011, 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2014, Sony Mobile Communications Inc.
 *
 * nabu overlay (6.18.y): keep the power key usable with s2idle.
 *  - The PMIC asserts KPDPWR for ~180ms right after suspend-to-idle entry
 *    although nobody touches the tablet.  With the IRQ armed as a wakeup
 *    source that aborts the suspend, so clear the PON interrupt enable in
 *    .suspend() for qcom,suspend-irq-mask-ms and re-enable it from a timer
 *    after reading PON_RT_STS.
 *  - The press that wakes the system must not reach userspace, or logind
 *    suspends again as soon as the resume completes.
 * Details and measurements: nabu-linux-overlay/docs/port-6.18.md
 */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include <linux/suspend.h>
#include <linux/timer.h>

#define PON_REV2			0x01

#define PON_SUBTYPE			0x05

#define PON_SUBTYPE_PRIMARY		0x01
#define PON_SUBTYPE_SECONDARY		0x02
#define PON_SUBTYPE_1REG		0x03
#define PON_SUBTYPE_GEN2_PRIMARY	0x04
#define PON_SUBTYPE_GEN2_SECONDARY	0x05
#define PON_SUBTYPE_GEN3_PBS		0x08
#define PON_SUBTYPE_GEN3_HLOS		0x09

#define PON_RT_STS			0x10
#define  PON_KPDPWR_N_SET		BIT(0)
#define  PON_RESIN_N_SET		BIT(1)
#define  PON_GEN3_RESIN_N_SET		BIT(6)
#define  PON_GEN3_KPDPWR_N_SET		BIT(7)

#define PON_PS_HOLD_RST_CTL		0x5a
#define PON_PS_HOLD_RST_CTL2		0x5b
#define  PON_PS_HOLD_ENABLE		BIT(7)
#define  PON_PS_HOLD_TYPE_MASK		0x0f
#define  PON_PS_HOLD_TYPE_WARM_RESET	1
#define  PON_PS_HOLD_TYPE_SHUTDOWN	4
#define  PON_PS_HOLD_TYPE_HARD_RESET	7

#define PON_INT_LATCHED_CLR		0x14
#define PON_INT_EN_SET			0x15
#define PON_INT_EN_CLR			0x16

#define PON_PULL_CTL			0x70
#define  PON_KPDPWR_PULL_UP		BIT(1)
#define  PON_RESIN_PULL_UP		BIT(0)

#define PON_DBC_CTL			0x71
#define  PON_DBC_DELAY_MASK_GEN1	0x7
#define  PON_DBC_DELAY_MASK_GEN2	0xf
#define  PON_DBC_SHIFT_GEN1		6
#define  PON_DBC_SHIFT_GEN2		14

struct pm8941_data {
	unsigned int	pull_up_bit;
	unsigned int	status_bit;
	bool		supports_ps_hold_poff_config;
	bool		supports_debounce_config;
	bool		has_pon_pbs;
	bool		wakeup_source_default;
	const char	*name;
	const char	*phys;
};

struct pm8941_pwrkey {
	struct device *dev;
	int irq;
	u32 baseaddr;
	u32 pon_pbs_baseaddr;
	struct regmap *regmap;
	struct input_dev *input;

	/* nabu: mask/consume the key across the suspend-entry transient */
	u32 suspend_mask_ms;
	struct timer_list unmask_timer;
	bool irq_masked;
	bool suspended;
	bool swallow_release;
	ktime_t drop_until;

	unsigned int revision;
	unsigned int subtype;
	struct notifier_block reboot_notifier;

	u32 code;
	u32 sw_debounce_time_us;
	ktime_t sw_debounce_end_time;
	bool last_status;
	const struct pm8941_data *data;
};

static int pm8941_reboot_notify(struct notifier_block *nb,
				unsigned long code, void *unused)
{
	struct pm8941_pwrkey *pwrkey = container_of(nb, struct pm8941_pwrkey,
						    reboot_notifier);
	unsigned int enable_reg;
	unsigned int reset_type;
	int error;

	/* PMICs with revision 0 have the enable bit in same register as ctrl */
	if (pwrkey->revision == 0)
		enable_reg = PON_PS_HOLD_RST_CTL;
	else
		enable_reg = PON_PS_HOLD_RST_CTL2;

	error = regmap_update_bits(pwrkey->regmap,
				   pwrkey->baseaddr + enable_reg,
				   PON_PS_HOLD_ENABLE,
				   0);
	if (error)
		dev_err(pwrkey->dev,
			"unable to clear ps hold reset enable: %d\n",
			error);

	/*
	 * Updates of PON_PS_HOLD_ENABLE requires 3 sleep cycles between
	 * writes.
	 */
	usleep_range(100, 1000);

	switch (code) {
	case SYS_HALT:
	case SYS_POWER_OFF:
		reset_type = PON_PS_HOLD_TYPE_SHUTDOWN;
		break;
	case SYS_RESTART:
	default:
		if (reboot_mode == REBOOT_WARM)
			reset_type = PON_PS_HOLD_TYPE_WARM_RESET;
		else
			reset_type = PON_PS_HOLD_TYPE_HARD_RESET;
		break;
	}

	error = regmap_update_bits(pwrkey->regmap,
				   pwrkey->baseaddr + PON_PS_HOLD_RST_CTL,
				   PON_PS_HOLD_TYPE_MASK,
				   reset_type);
	if (error)
		dev_err(pwrkey->dev, "unable to set ps hold reset type: %d\n",
			error);

	error = regmap_update_bits(pwrkey->regmap,
				   pwrkey->baseaddr + enable_reg,
				   PON_PS_HOLD_ENABLE,
				   PON_PS_HOLD_ENABLE);
	if (error)
		dev_err(pwrkey->dev, "unable to re-set enable: %d\n", error);

	return NOTIFY_DONE;
}

/* nabu: SDAM marker used by the suspend investigation, see nabu-suspend.c */
void nabu_suspend_mark(u8 v);

static irqreturn_t pm8941_pwrkey_irq(int irq, void *_data)
{
	struct pm8941_pwrkey *pwrkey = _data;
	unsigned int sts;
	int err;

	if (pwrkey->sw_debounce_time_us) {
		if (ktime_before(ktime_get(), pwrkey->sw_debounce_end_time)) {
			dev_dbg(pwrkey->dev,
				"ignoring key event received before debounce end %lld us\n",
				ktime_to_us(pwrkey->sw_debounce_end_time));
			return IRQ_HANDLED;
		}
	}

	err = regmap_read(pwrkey->regmap, pwrkey->baseaddr + PON_RT_STS, &sts);
	if (err)
		return IRQ_HANDLED;

	sts &= pwrkey->data->status_bit;

	/*
	 * nabu: the press that wakes the system must not reach userspace, or
	 * logind suspends again as soon as the resume completes (the screen
	 * flashes and goes dark again).  Consume the press and its release
	 * while suspended and for a short while after resume.
	 */
	if (pwrkey->suspended ||
	    (pwrkey->drop_until && ktime_before(ktime_get(), pwrkey->drop_until)) ||
	    pwrkey->swallow_release) {
		pwrkey->swallow_release = sts;
		pwrkey->last_status = sts;
		return IRQ_HANDLED;
	}

	if (pwrkey->sw_debounce_time_us && !sts)
		pwrkey->sw_debounce_end_time = ktime_add_us(ktime_get(),
						pwrkey->sw_debounce_time_us);

	/*
	 * Simulate a press event in case a release event occurred without a
	 * corresponding press event.
	 */
	if (!pwrkey->last_status && !sts) {
		input_report_key(pwrkey->input, pwrkey->code, 1);
		input_sync(pwrkey->input);
	}
	pwrkey->last_status = sts;

	input_report_key(pwrkey->input, pwrkey->code, sts);
	input_sync(pwrkey->input);

	return IRQ_HANDLED;
}

static int pm8941_pwrkey_sw_debounce_init(struct pm8941_pwrkey *pwrkey)
{
	unsigned int val, addr, mask;
	int error;

	if (pwrkey->data->has_pon_pbs && !pwrkey->pon_pbs_baseaddr) {
		dev_err(pwrkey->dev,
			"PON_PBS address missing, can't read HW debounce time\n");
		return 0;
	}

	if (pwrkey->pon_pbs_baseaddr)
		addr = pwrkey->pon_pbs_baseaddr + PON_DBC_CTL;
	else
		addr = pwrkey->baseaddr + PON_DBC_CTL;
	error = regmap_read(pwrkey->regmap, addr, &val);
	if (error)
		return error;

	if (pwrkey->subtype >= PON_SUBTYPE_GEN2_PRIMARY)
		mask = 0xf;
	else
		mask = 0x7;

	pwrkey->sw_debounce_time_us =
		2 * USEC_PER_SEC / (1 << (mask - (val & mask)));

	dev_dbg(pwrkey->dev, "SW debounce time = %u us\n",
		pwrkey->sw_debounce_time_us);

	return 0;
}

static void pm8941_pwrkey_mask(struct pm8941_pwrkey *pwrkey, bool mask)
{
	u8 bit = pwrkey->data->status_bit;

	if (mask) {
		regmap_write(pwrkey->regmap,
			     pwrkey->baseaddr + PON_INT_EN_CLR, bit);
		regmap_write(pwrkey->regmap,
			     pwrkey->baseaddr + PON_INT_LATCHED_CLR, bit);
	} else {
		regmap_write(pwrkey->regmap,
			     pwrkey->baseaddr + PON_INT_LATCHED_CLR, bit);
		regmap_write(pwrkey->regmap,
			     pwrkey->baseaddr + PON_INT_EN_SET, bit);
	}

	pwrkey->irq_masked = mask;
}

static void pm8941_pwrkey_unmask(struct timer_list *t)
{
	struct pm8941_pwrkey *pwrkey = timer_container_of(pwrkey, t,
							  unmask_timer);
	unsigned int sts = 0;

	if (!pwrkey->irq_masked)
		return;

	regmap_read(pwrkey->regmap, pwrkey->baseaddr + PON_RT_STS, &sts);
	pm8941_pwrkey_mask(pwrkey, false);
	dev_dbg(pwrkey->dev, "key IRQ unmasked, RT_STS=0x%x\n", sts);
}

static int pm8941_pwrkey_suspend(struct device *dev)
{
	struct pm8941_pwrkey *pwrkey = dev_get_drvdata(dev);

	if (!device_may_wakeup(dev))
		return 0;

	/* nabu: marker 0x07, the device suspend phase reached the PMIC */
	nabu_suspend_mark(0x07);

	/*
	 * nabu: a platform "mem" suspend is woken by the key IRQ itself; do
	 * not mask it, the PMIC transient only costs one retry (see
	 * nabu-suspend.c).  suspend-to-idle masks it instead.
	 */
	if (pm_suspend_target_state == PM_SUSPEND_MEM) {
		pwrkey->suspended = true;
		pwrkey->drop_until = 0;
		enable_irq_wake(pwrkey->irq);
		return 0;
	}

	enable_irq_wake(pwrkey->irq);

	if (pwrkey->suspend_mask_ms) {
		/*
		 * nabu: the PMIC asserts KPDPWR for ~180ms right after
		 * suspend-to-idle entry although nobody touches the tablet, and
		 * with the wakeup armed that ends the suspend immediately
		 * (measured on 6.14 and 6.18).  Clear the PON interrupt enable
		 * so the transient never becomes an IRQ, and enable it again
		 * once the line has settled.  disable_irq() cannot be used for
		 * this: an armed IRQ is not masked by the IRQ core.
		 */
		pwrkey->suspended = true;
		pwrkey->drop_until = 0;
		pm8941_pwrkey_mask(pwrkey, true);
		mod_timer(&pwrkey->unmask_timer,
			  jiffies + msecs_to_jiffies(pwrkey->suspend_mask_ms));
	}

	return 0;
}

static int pm8941_pwrkey_resume(struct device *dev)
{
	struct pm8941_pwrkey *pwrkey = dev_get_drvdata(dev);


	if (pwrkey->suspend_mask_ms) {
		timer_delete_sync(&pwrkey->unmask_timer);
		if (pwrkey->irq_masked)
			pm8941_pwrkey_mask(pwrkey, false);

		pwrkey->suspended = false;
		pwrkey->swallow_release = pwrkey->last_status;
		pwrkey->drop_until = ktime_add_ms(ktime_get(), 500);
	}

	if (device_may_wakeup(dev))
		disable_irq_wake(pwrkey->irq);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(pm8941_pwr_key_pm_ops,
				pm8941_pwrkey_suspend, pm8941_pwrkey_resume);

#define NABU_SDAM_MARK_REG	0xb17e
/*
 * nabu: the suspend path is debugged from the SDAM because the kernel log of
 * a failed suspend dies with the reset.  Keep the last 16 marks in a ring so
 * the sequence (and not just the last step) survives.
 */
#define NABU_SDAM_RING_REG	0xb160
#define NABU_SDAM_RING_LEN	16

static struct pm8941_pwrkey *nabu_pwrkey;

void nabu_suspend_mark(u8 v)
{
	static u8 idx;

	if (!nabu_pwrkey)
		return;

	regmap_write(nabu_pwrkey->regmap, NABU_SDAM_MARK_REG, v);
	regmap_write(nabu_pwrkey->regmap,
		     NABU_SDAM_RING_REG + (idx++ % NABU_SDAM_RING_LEN), v);
}
EXPORT_SYMBOL_GPL(nabu_suspend_mark);

/* nabu: write a 32 bit value to the SDAM ring area byte by byte */
void nabu_suspend_word(u8 off, u32 v)
{
	int i;

	if (!nabu_pwrkey)
		return;

	for (i = 0; i < 4; i++)
		regmap_write(nabu_pwrkey->regmap, 0xb100 + off + i,
			     (v >> (8 * i)) & 0xff);
}
EXPORT_SYMBOL_GPL(nabu_suspend_word);

/* nabu: virq of the power key, used by the platform mem suspend driver */
unsigned int nabu_pwrkey_irq(void)
{
	return nabu_pwrkey ? nabu_pwrkey->irq : 0;
}
EXPORT_SYMBOL_GPL(nabu_pwrkey_irq);

/*
 * nabu: the press that ends a platform mem suspend is a real press and has to
 * reach userspace, or the SoC resumes but the screen stays dark.
 */
void nabu_pwrkey_allow_wake(void)
{
	if (!nabu_pwrkey)
		return;

	nabu_pwrkey->suspended = false;
	nabu_pwrkey->swallow_release = false;
	nabu_pwrkey->drop_until = 0;
}
EXPORT_SYMBOL_GPL(nabu_pwrkey_allow_wake);

/* nabu: mask/unmask the PON interrupt without going through the IRQ core */
void nabu_pwrkey_irq_off(void)
{
	if (nabu_pwrkey)
		pm8941_pwrkey_mask(nabu_pwrkey, true);
}
EXPORT_SYMBOL_GPL(nabu_pwrkey_irq_off);

void nabu_pwrkey_irq_on(void)
{
	if (nabu_pwrkey)
		pm8941_pwrkey_mask(nabu_pwrkey, false);
}
EXPORT_SYMBOL_GPL(nabu_pwrkey_irq_on);

/* nabu: is the key line still asserted (the entry transient lasts ~200ms) */
bool nabu_pwrkey_pressed(void)
{
	u32 sts = 0;

	if (!nabu_pwrkey)
		return false;

	regmap_read(nabu_pwrkey->regmap, nabu_pwrkey->baseaddr + PON_RT_STS, &sts);
	return sts & nabu_pwrkey->data->status_bit;
}
EXPORT_SYMBOL_GPL(nabu_pwrkey_pressed);

/*
 * nabu: drain a latched key event by dispatching the child IRQ only.  Going
 * through the chained SPMI summary handler would EOI the parent PDC pin at
 * the GIC, and an EOI for an interrupt that is not active corrupts the GIC
 * running priority on GICv3, after which wakeups are never delivered again.
 */
void nabu_pwrkey_drain(void)
{
	if (nabu_pwrkey)
		generic_handle_irq(nabu_pwrkey->irq);
}
EXPORT_SYMBOL_GPL(nabu_pwrkey_drain);

/* nabu: PON interrupt enable / latched state, for the SDAM black box */
u32 nabu_pwrkey_en(void)
{
	u32 v = 0;

	if (nabu_pwrkey)
		regmap_read(nabu_pwrkey->regmap,
			    nabu_pwrkey->baseaddr + PON_INT_EN_SET, &v);
	return v;
}
EXPORT_SYMBOL_GPL(nabu_pwrkey_en);

u32 nabu_pwrkey_latched(void)
{
	u32 v = 0;

	if (nabu_pwrkey)
		regmap_read(nabu_pwrkey->regmap,
			    nabu_pwrkey->baseaddr + 0x11 /* LATCHED_STS */, &v);
	return v;
}
EXPORT_SYMBOL_GPL(nabu_pwrkey_latched);

/* nabu: re-arm the key wakeup for a platform mem suspend entry */
void nabu_pwrkey_rearm(void)
{
	static bool wake_armed;
	int ret;

	if (!nabu_pwrkey)
		return;

	/* force the chip's set_wake() to run again; only drop the reference we
	 * took ourselves, wake_depth is also held by the driver's suspend */
	if (wake_armed)
		disable_irq_wake(nabu_pwrkey->irq);
	ret = enable_irq_wake(nabu_pwrkey->irq);
	wake_armed = !ret;
	if (ret)
		pr_info("nabu: pwrkey irq %u: enable_irq_wake failed (%d)\n",
			nabu_pwrkey->irq, ret);

	/*
	 * The IRQ core masks the PON interrupt in the PMIC (QPNPINT EN_CLR)
	 * when it reports the wakeup; make sure it is enabled again, or the
	 * next entry cannot be woken by the key at all (the charger IRQ,
	 * which is never disabled, still wakes it).
	 */
	pm8941_pwrkey_mask(nabu_pwrkey, false);
}
EXPORT_SYMBOL_GPL(nabu_pwrkey_rearm);

static int pm8941_pwrkey_probe(struct platform_device *pdev)
{
	struct pm8941_pwrkey *pwrkey;
	bool pull_up, wakeup;
	struct device *parent;
	struct device_node *regmap_node;
	const __be32 *addr;
	u32 req_delay, mask, delay_shift;
	int error;

	if (of_property_read_u32(pdev->dev.of_node, "debounce", &req_delay))
		req_delay = 15625;

	if (req_delay > 2000000 || req_delay == 0) {
		dev_err(&pdev->dev, "invalid debounce time: %u\n", req_delay);
		return -EINVAL;
	}

	pull_up = of_property_read_bool(pdev->dev.of_node, "bias-pull-up");

	pwrkey = devm_kzalloc(&pdev->dev, sizeof(*pwrkey), GFP_KERNEL);
	if (!pwrkey)
		return -ENOMEM;

	pwrkey->dev = &pdev->dev;
	pwrkey->data = of_device_get_match_data(&pdev->dev);

	parent = pdev->dev.parent;
	regmap_node = pdev->dev.of_node;
	pwrkey->regmap = dev_get_regmap(parent, NULL);
	if (!pwrkey->regmap) {
		regmap_node = parent->of_node;
		/*
		 * We failed to get regmap for parent. Let's see if we are
		 * a child of pon node and read regmap and reg from its
		 * parent.
		 */
		pwrkey->regmap = dev_get_regmap(parent->parent, NULL);
		if (!pwrkey->regmap) {
			dev_err(&pdev->dev, "failed to locate regmap\n");
			return -ENODEV;
		}
	}

	addr = of_get_address(regmap_node, 0, NULL, NULL);
	if (!addr) {
		dev_err(&pdev->dev, "reg property missing\n");
		return -EINVAL;
	}
	pwrkey->baseaddr = be32_to_cpup(addr);

	if (pwrkey->data->has_pon_pbs) {
		/* PON_PBS base address is optional */
		addr = of_get_address(regmap_node, 1, NULL, NULL);
		if (addr)
			pwrkey->pon_pbs_baseaddr = be32_to_cpup(addr);
	}

	pwrkey->irq = platform_get_irq(pdev, 0);
	if (pwrkey->irq < 0)
		return pwrkey->irq;

	error = regmap_read(pwrkey->regmap, pwrkey->baseaddr + PON_REV2,
			    &pwrkey->revision);
	if (error) {
		dev_err(&pdev->dev, "failed to read revision: %d\n", error);
		return error;
	}

	error = regmap_read(pwrkey->regmap, pwrkey->baseaddr + PON_SUBTYPE,
			    &pwrkey->subtype);
	if (error) {
		dev_err(&pdev->dev, "failed to read subtype: %d\n", error);
		return error;
	}

	error = of_property_read_u32(pdev->dev.of_node, "linux,code",
				     &pwrkey->code);
	if (error) {
		dev_dbg(&pdev->dev,
			"no linux,code assuming power (%d)\n", error);
		pwrkey->code = KEY_POWER;
	}

	pwrkey->input = devm_input_allocate_device(&pdev->dev);
	if (!pwrkey->input) {
		dev_dbg(&pdev->dev, "unable to allocate input device\n");
		return -ENOMEM;
	}

	input_set_capability(pwrkey->input, EV_KEY, pwrkey->code);

	pwrkey->input->name = pwrkey->data->name;
	pwrkey->input->phys = pwrkey->data->phys;

	if (pwrkey->data->supports_debounce_config) {
		if (pwrkey->subtype >= PON_SUBTYPE_GEN2_PRIMARY) {
			mask = PON_DBC_DELAY_MASK_GEN2;
			delay_shift = PON_DBC_SHIFT_GEN2;
		} else {
			mask = PON_DBC_DELAY_MASK_GEN1;
			delay_shift = PON_DBC_SHIFT_GEN1;
		}

		req_delay = (req_delay << delay_shift) / USEC_PER_SEC;
		req_delay = ilog2(req_delay);

		error = regmap_update_bits(pwrkey->regmap,
					   pwrkey->baseaddr + PON_DBC_CTL,
					   mask,
					   req_delay);
		if (error) {
			dev_err(&pdev->dev, "failed to set debounce: %d\n",
				error);
			return error;
		}
	}

	error = pm8941_pwrkey_sw_debounce_init(pwrkey);
	if (error)
		return error;

	if (pwrkey->data->pull_up_bit) {
		error = regmap_update_bits(pwrkey->regmap,
					   pwrkey->baseaddr + PON_PULL_CTL,
					   pwrkey->data->pull_up_bit,
					   pull_up ? pwrkey->data->pull_up_bit :
						     0);
		if (error) {
			dev_err(&pdev->dev, "failed to set pull: %d\n", error);
			return error;
		}
	}

	error = device_property_read_u32(&pdev->dev, "qcom,suspend-irq-mask-ms",
					 &pwrkey->suspend_mask_ms);
	if (error)
		pwrkey->suspend_mask_ms = 0;

	timer_setup(&pwrkey->unmask_timer, pm8941_pwrkey_unmask, 0);

	error = devm_request_threaded_irq(&pdev->dev, pwrkey->irq,
					  NULL, pm8941_pwrkey_irq,
					  IRQF_ONESHOT,
					  pwrkey->data->name, pwrkey);
	if (error) {
		dev_err(&pdev->dev, "failed requesting IRQ: %d\n", error);
		return error;
	}

	error = input_register_device(pwrkey->input);
	if (error) {
		dev_err(&pdev->dev, "failed to register input device: %d\n",
			error);
		return error;
	}

	if (pwrkey->data->supports_ps_hold_poff_config) {
		pwrkey->reboot_notifier.notifier_call = pm8941_reboot_notify;
		error = register_reboot_notifier(&pwrkey->reboot_notifier);
		if (error) {
			dev_err(&pdev->dev, "failed to register reboot notifier: %d\n",
				error);
			return error;
		}
	}

	wakeup = pwrkey->data->wakeup_source_default ||
		of_property_read_bool(pdev->dev.of_node, "wakeup-source");

	platform_set_drvdata(pdev, pwrkey);
	device_init_wakeup(&pdev->dev, wakeup);

	nabu_pwrkey = pwrkey;

	return 0;
}

static void pm8941_pwrkey_remove(struct platform_device *pdev)
{
	struct pm8941_pwrkey *pwrkey = platform_get_drvdata(pdev);

	if (pwrkey->data->supports_ps_hold_poff_config)
		unregister_reboot_notifier(&pwrkey->reboot_notifier);
}

static const struct pm8941_data pwrkey_data = {
	.pull_up_bit = PON_KPDPWR_PULL_UP,
	.status_bit = PON_KPDPWR_N_SET,
	.name = "pm8941_pwrkey",
	.phys = "pm8941_pwrkey/input0",
	.supports_ps_hold_poff_config = true,
	.supports_debounce_config = true,
	.has_pon_pbs = false,
	.wakeup_source_default = true,
};

static const struct pm8941_data resin_data = {
	.pull_up_bit = PON_RESIN_PULL_UP,
	.status_bit = PON_RESIN_N_SET,
	.name = "pm8941_resin",
	.phys = "pm8941_resin/input0",
	.supports_ps_hold_poff_config = true,
	.supports_debounce_config = true,
	.has_pon_pbs = false,
	.wakeup_source_default = false,
};

static const struct pm8941_data pon_gen3_pwrkey_data = {
	.status_bit = PON_GEN3_KPDPWR_N_SET,
	.name = "pmic_pwrkey",
	.phys = "pmic_pwrkey/input0",
	.supports_ps_hold_poff_config = false,
	.supports_debounce_config = false,
	.has_pon_pbs = true,
	.wakeup_source_default = true,
};

static const struct pm8941_data pon_gen3_resin_data = {
	.status_bit = PON_GEN3_RESIN_N_SET,
	.name = "pmic_resin",
	.phys = "pmic_resin/input0",
	.supports_ps_hold_poff_config = false,
	.supports_debounce_config = false,
	.has_pon_pbs = true,
	.wakeup_source_default = false,
};

static const struct of_device_id pm8941_pwr_key_id_table[] = {
	{ .compatible = "qcom,pm8941-pwrkey", .data = &pwrkey_data },
	{ .compatible = "qcom,pm8941-resin", .data = &resin_data },
	{ .compatible = "qcom,pmk8350-pwrkey", .data = &pon_gen3_pwrkey_data },
	{ .compatible = "qcom,pmk8350-resin", .data = &pon_gen3_resin_data },
	{ }
};
MODULE_DEVICE_TABLE(of, pm8941_pwr_key_id_table);

static struct platform_driver pm8941_pwrkey_driver = {
	.probe = pm8941_pwrkey_probe,
	.remove = pm8941_pwrkey_remove,
	.driver = {
		.name = "pm8941-pwrkey",
		.pm = pm_sleep_ptr(&pm8941_pwr_key_pm_ops),
		.of_match_table = of_match_ptr(pm8941_pwr_key_id_table),
	},
};
module_platform_driver(pm8941_pwrkey_driver);

MODULE_DESCRIPTION("PM8941 Power Key driver");
MODULE_LICENSE("GPL v2");
