// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017-18 Linaro Limited

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/reboot-mode.h>
#include <linux/regmap.h>

#define PON_SOFT_RB_SPARE		0x8f

/*
 * nabu: PS_HOLD reset configuration.  The PMIC applies the power-off type
 * programmed here when PS_HOLD is pulled low, so programming "warm reset"
 * turns every restart (including the one after a panic) into a warm reset,
 * which keeps the DRAM contents - and with them the pstore/ramoops buffer -
 * across the reboot.  Register layout and sequence follow the vendor's
 * qpnp_pon_reset_config().
 */
#define PON_PS_HOLD_RST_CTL		0x5a
#define PON_PS_HOLD_RST_CTL2		0x5b
#define PON_POWER_OFF_MASK		0xf
#define PON_RESET_EN_BIT		BIT(7)
#define PON_POWER_OFF_WARM_RESET	0x01

extern void nabu_suspend_mark(u8 v);
extern void nabu_suspend_word(u8 off, u32 v);

static bool warm_reset = true;
module_param(warm_reset, bool, 0644);
MODULE_PARM_DESC(warm_reset, "Use a warm reset to keep DRAM (default: 1)");

#define GEN1_REASON_SHIFT		2
#define GEN2_REASON_SHIFT		1

#define NO_REASON_SHIFT			0

struct qcom_pon {
	struct device *dev;
	struct regmap *regmap;
	u32 baseaddr;
	struct reboot_mode_driver reboot_mode;
	long reason_shift;
};

static int qcom_pon_reboot_mode_write(struct reboot_mode_driver *reboot,
				    unsigned int magic)
{
	struct qcom_pon *pon = container_of
			(reboot, struct qcom_pon, reboot_mode);
	int ret;

	ret = regmap_update_bits(pon->regmap,
				 pon->baseaddr + PON_SOFT_RB_SPARE,
				 GENMASK(7, pon->reason_shift),
				 magic << pon->reason_shift);
	if (ret < 0)
		dev_err(pon->dev, "update reboot mode bits failed\n");

	return ret;
}

static int qcom_pon_restart(struct sys_off_data *data)
{
	struct qcom_pon *pon = data->cb_data;
	int ret;

	if (!warm_reset)
		return NOTIFY_DONE;

	/* disable the reset, program the type, then enable it again */
	ret = regmap_update_bits(pon->regmap, pon->baseaddr + PON_PS_HOLD_RST_CTL2,
				 PON_RESET_EN_BIT, 0);
	if (ret)
		return NOTIFY_DONE;

	udelay(500);

	ret = regmap_update_bits(pon->regmap, pon->baseaddr + PON_PS_HOLD_RST_CTL,
				 PON_POWER_OFF_MASK, PON_POWER_OFF_WARM_RESET);
	if (ret)
		return NOTIFY_DONE;

	ret = regmap_update_bits(pon->regmap, pon->baseaddr + PON_PS_HOLD_RST_CTL2,
				 PON_RESET_EN_BIT, PON_RESET_EN_BIT);
	if (ret)
		return NOTIFY_DONE;

	dev_info(pon->dev, "warm reset armed (DRAM kept for pstore)\n");

	/*
	 * Record what the PMIC reports right after programming in the PMIC
	 * SDAM: it is always-on RAM, so the value survives whatever reset we
	 * end up doing and can be checked from the next boot.
	 */
	if (nabu_suspend_word) {
		unsigned int ctl = 0, ctl2 = 0;

		regmap_read(pon->regmap, pon->baseaddr + PON_PS_HOLD_RST_CTL, &ctl);
		regmap_read(pon->regmap, pon->baseaddr + PON_PS_HOLD_RST_CTL2, &ctl2);
		nabu_suspend_mark(0xa5);
		nabu_suspend_word(0x54, (ctl & 0xff) | ((ctl2 & 0xff) << 8) | 0x10000);
	}

	return NOTIFY_DONE;
}

static int qcom_pon_probe(struct platform_device *pdev)
{
	struct qcom_pon *pon;
	long reason_shift;
	int error;

	pon = devm_kzalloc(&pdev->dev, sizeof(*pon), GFP_KERNEL);
	if (!pon)
		return -ENOMEM;

	pon->dev = &pdev->dev;

	pon->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!pon->regmap) {
		dev_err(&pdev->dev, "failed to locate regmap\n");
		return -ENODEV;
	}

	error = of_property_read_u32(pdev->dev.of_node, "reg",
				     &pon->baseaddr);
	if (error)
		return error;

	reason_shift = (long)of_device_get_match_data(&pdev->dev);

	if (reason_shift != NO_REASON_SHIFT) {
		pon->reboot_mode.dev = &pdev->dev;
		pon->reason_shift = reason_shift;
		pon->reboot_mode.write = qcom_pon_reboot_mode_write;
		error = devm_reboot_mode_register(&pdev->dev, &pon->reboot_mode);
		if (error) {
			dev_err(&pdev->dev, "can't register reboot mode\n");
			return error;
		}
	}

	platform_set_drvdata(pdev, pon);

	{
		unsigned int ctl = 0, ctl2 = 0;

		regmap_read(pon->regmap, pon->baseaddr + PON_PS_HOLD_RST_CTL, &ctl);
		regmap_read(pon->regmap, pon->baseaddr + PON_PS_HOLD_RST_CTL2, &ctl2);
		dev_info(&pdev->dev,
			 "PS_HOLD_RST_CTL=%#x CTL2=%#x (type=%#x en=%d)\n",
			 ctl, ctl2, ctl & PON_POWER_OFF_MASK, !!(ctl2 & PON_RESET_EN_BIT));
	}

	/*
	 * Run before msm-poweroff (priority 128) so the type is programmed
	 * before PS_HOLD goes low.
	 */
	error = devm_register_sys_off_handler(&pdev->dev, SYS_OFF_MODE_RESTART,
					      200, qcom_pon_restart, pon);
	if (error)
		dev_warn(&pdev->dev, "no warm reset support: %d\n", error);

	return devm_of_platform_populate(&pdev->dev);
}

static const struct of_device_id qcom_pon_id_table[] = {
	{ .compatible = "qcom,pm8916-pon", .data = (void *)GEN1_REASON_SHIFT },
	{ .compatible = "qcom,pm8941-pon", .data = (void *)NO_REASON_SHIFT },
	{ .compatible = "qcom,pms405-pon", .data = (void *)GEN1_REASON_SHIFT },
	{ .compatible = "qcom,pm8998-pon", .data = (void *)GEN2_REASON_SHIFT },
	{ .compatible = "qcom,pmk8350-pon", .data = (void *)GEN2_REASON_SHIFT },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_pon_id_table);

static struct platform_driver qcom_pon_driver = {
	.probe = qcom_pon_probe,
	.driver = {
		.name = "qcom-pon",
		.of_match_table = qcom_pon_id_table,
	},
};
module_platform_driver(qcom_pon_driver);

MODULE_DESCRIPTION("Qualcomm Power On driver");
MODULE_LICENSE("GPL v2");
