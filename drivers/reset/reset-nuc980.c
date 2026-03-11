// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020, Veiko Rütter <rebane@alkohol.ee>
 */

#include <linux/err.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>
#include <linux/types.h>

#define REG_AHBIPRST  0x060
#define REG_APBIPRST0 0x064
#define REG_APBIPRST1 0x068
#define REG_REGWPCTL  0x1fc

struct nuc980_reset {
	struct regmap               *regmap;
	spinlock_t                  lock;
	struct reset_controller_dev rcdev;
	struct notifier_block       restart_nb;
	struct device               *dev;
};

#define to_nuc980_reset(p)		\
	container_of((p), struct nuc980_reset, rcdev)

static int nuc980_reset_to_reg(unsigned long idx, u32 *reg)
{
	if (idx < 32)
		*reg = REG_AHBIPRST;
	else if (idx < 64)
		*reg = REG_APBIPRST0;
	else if (idx < 96)
		*reg = REG_APBIPRST1;
	else
		return -EINVAL;

	return 0;
}

static int nuc980_reset_of_xlate(struct reset_controller_dev *rcdev,
			const struct of_phandle_args *reset_spec)
{
	if (reset_spec->args_count < 2 || reset_spec->args[1] >= 32)
		return -EINVAL;

	if (reset_spec->args[0] == REG_AHBIPRST)
		return reset_spec->args[1];

	if (reset_spec->args[0] == REG_APBIPRST0)
		return 32 + reset_spec->args[1];

	if (reset_spec->args[0] == REG_APBIPRST1)
		return 64 + reset_spec->args[1];

	return -EINVAL;
}

static int nuc980_reset_syscon_unlock(struct nuc980_reset *nreset)
{
	u32 val;
	int i;

	regmap_read(nreset->regmap, REG_REGWPCTL, &val);
	if (val == 0x01)
		return 0;

	for (i = 0; i < 16; i++) {
		regmap_write(nreset->regmap, REG_REGWPCTL, 0x59);
		regmap_write(nreset->regmap, REG_REGWPCTL, 0x16);
		regmap_write(nreset->regmap, REG_REGWPCTL, 0x88);
		regmap_read(nreset->regmap, REG_REGWPCTL, &val);
		if (val == 0x01)
			return 0;
	}

	return -EBUSY;
}

static int nuc980_reset_assert(struct reset_controller_dev *rcdev,
			     unsigned long id)
{
	struct nuc980_reset *nreset = to_nuc980_reset(rcdev);
	u32 reg;
	int ret;

	ret = nuc980_reset_to_reg(id, &reg);
	if (ret)
		return ret;

	spin_lock(&nreset->lock);

	/* disable write protect */
	ret = nuc980_reset_syscon_unlock(nreset);
	if (ret)
		goto unlock;

	ret = regmap_update_bits(nreset->regmap, reg,
					BIT(id & 0x1f), BIT(id & 0x1f));

	/* enable write protect */
	regmap_write(nreset->regmap, REG_REGWPCTL, 0x00);

unlock:
	spin_unlock(&nreset->lock);
	return ret;
}

static int nuc980_reset_deassert(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	struct nuc980_reset *nreset = to_nuc980_reset(rcdev);
	u32 reg;
	int ret;

	ret = nuc980_reset_to_reg(id, &reg);
	if (ret)
		return ret;

	spin_lock(&nreset->lock);

	/* disable write protect */
	ret = nuc980_reset_syscon_unlock(nreset);
	if (ret)
		goto unlock;

	ret = regmap_update_bits(nreset->regmap, reg, BIT(id & 0x1f), 0);

	/* enable write protect */
	regmap_write(nreset->regmap, REG_REGWPCTL, 0x00);

unlock:
	spin_unlock(&nreset->lock);
	return ret;
}

static int nuc980_reset_reset(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	struct nuc980_reset *nreset = to_nuc980_reset(rcdev);
	u32 reg;
	int ret;

	ret = nuc980_reset_to_reg(id, &reg);
	if (ret)
		return ret;

	spin_lock(&nreset->lock);

	/* disable write protect */
	ret = nuc980_reset_syscon_unlock(nreset);
	if (ret)
		goto unlock;

	regmap_update_bits(nreset->regmap, reg, BIT(id & 0x1f), BIT(id & 0x1f));
	udelay(10);
	ret = regmap_update_bits(nreset->regmap, reg, BIT(id & 0x1f), 0);

	/* enable write protect */
	regmap_write(nreset->regmap, REG_REGWPCTL, 0x00);

unlock:
	spin_unlock(&nreset->lock);
	return ret;
}

static int nuc980_reset_status(struct reset_controller_dev *rcdev,
			     unsigned long id)
{
	struct nuc980_reset *nreset = to_nuc980_reset(rcdev);
	u32 reg;
	u32 val;
	int ret;

	ret = nuc980_reset_to_reg(id, &reg);
	if (ret)
		return ret;

	ret = regmap_read(nreset->regmap, reg, &val);
	if (ret)
		return ret;

	return !!(val & BIT(id & 0x1f));
}

static const struct reset_control_ops nuc980_reset_ops = {
	.assert   = nuc980_reset_assert,
	.deassert = nuc980_reset_deassert,
	.reset    = nuc980_reset_reset,
	.status   = nuc980_reset_status,
};

static int nuc980_reset_restart_handler(struct notifier_block *nb,
				unsigned long action, void *data)
{
	struct nuc980_reset *nreset =
		container_of(nb, struct nuc980_reset, restart_nb);
	int ret;

	msleep(1000);
	ret = nuc980_reset_assert(&nreset->rcdev, 0);
	if (ret) {
		dev_err(nreset->dev, "unable to reset system\n");
		return NOTIFY_DONE;
	}

	mdelay(2000);

	return NOTIFY_DONE;
}

static int nuc980_reset_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct nuc980_reset *nreset;
	int ret;

	dev_set_name(dev, "regmap.nuc980-reset");

	nreset = devm_kzalloc(dev, sizeof(*nreset), GFP_KERNEL);
	if (!nreset) {
		dev_err(dev, "unable to allocate memory\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, nreset);
	nreset->dev = dev;

	nreset->regmap = syscon_regmap_lookup_by_phandle(np, "syscon");
	if (IS_ERR(nreset->regmap)) {
		dev_err(dev, "unable to get syscon region\n");
		return PTR_ERR(nreset->regmap);
	}

	spin_lock_init(&nreset->lock);

	nreset->rcdev.nr_resets        = 96;
	nreset->rcdev.dev              = dev;
	nreset->rcdev.ops              = &nuc980_reset_ops;
	nreset->rcdev.of_reset_n_cells = 2;
	nreset->rcdev.of_xlate         = nuc980_reset_of_xlate;
	nreset->rcdev.of_node          = np;
	nreset->rcdev.owner            = THIS_MODULE;

	ret = devm_reset_controller_register(dev, &nreset->rcdev);
	if (ret) {
		dev_err(dev, "unable to register reset controller\n");
		return ret;
	}

	nreset->restart_nb.notifier_call = nuc980_reset_restart_handler;
	nreset->restart_nb.priority = 128;

	ret = register_restart_handler(&nreset->restart_nb);
	if (ret)
		dev_warn(dev, "unable to register restart handler\n");

	dev_info(dev, "initialized\n");

	return 0;
}

static const struct of_device_id nuc980_reset_dt_ids[] = {
	{ .compatible = "nuvoton,nuc980-reset", },
	{ },
};
MODULE_DEVICE_TABLE(of, nuc980_reset_dt_ids);

static struct platform_driver nuc980_reset_driver = {
	.probe	= nuc980_reset_probe,
	.driver = {
		.name		= "nuc980-reset",
		.of_match_table	= nuc980_reset_dt_ids,
	},
};

static int __init nuc980_reset_init(void)
{
	return platform_driver_register(&nuc980_reset_driver);
}
arch_initcall(nuc980_reset_init);
