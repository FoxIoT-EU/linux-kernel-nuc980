// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020, Veiko Rütter <rebane@alkohol.ee>
 *
 */

#include <linux/init.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/usb/role.h>

#define REG_PWRON          0x004
#define   PWRON_USBID      (1 << 16)
#define REG_MISCFCR        0x030
#define   MISCFCR_USRHDSEN (1 << 11)
#define REG_REGWPCTL       0x1fc

struct nuc980_role_sw {
	struct device          *dev;
	struct regmap          *regmap;
	struct usb_role_switch *role_sw;
	spinlock_t             lock;
};

static const struct software_node nuc980_role_sw_node = {
	"nuc980-role-sw",
};

static int nuc980_role_sw_syscon_unlock(struct nuc980_role_sw *nrl)
{
	u32 val;
	int i;

	regmap_read(nrl->regmap, REG_REGWPCTL, &val);
	if (val == 0x01)
		return 0;

	for (i = 0; i < 16; i++) {
		regmap_write(nrl->regmap, REG_REGWPCTL, 0x59);
		regmap_write(nrl->regmap, REG_REGWPCTL, 0x16);
		regmap_write(nrl->regmap, REG_REGWPCTL, 0x88);
		regmap_read(nrl->regmap, REG_REGWPCTL, &val);
		if (val == 0x01)
			return 0;
	}

	return -EBUSY;
}

static int nuc980_role_sw_set(struct usb_role_switch *sw, enum usb_role role)
{
	struct nuc980_role_sw *nrl = usb_role_switch_get_drvdata(sw);
	int ret;

	spin_lock(&nrl->lock);
	/* disable write protect */
	ret = nuc980_role_sw_syscon_unlock(nrl);
	if (ret)
		goto unlock;

	ret = regmap_update_bits(nrl->regmap, REG_MISCFCR,
					MISCFCR_USRHDSEN, MISCFCR_USRHDSEN);
	if (ret)
		goto unlock;

	if (role == USB_ROLE_HOST) {
		ret = regmap_update_bits(nrl->regmap, REG_PWRON,
					PWRON_USBID, PWRON_USBID);
	} else if (role == USB_ROLE_DEVICE) {
		ret = regmap_update_bits(nrl->regmap, REG_PWRON,
					PWRON_USBID, 0);
	}

	/* enable write protect */
	regmap_write(nrl->regmap, REG_REGWPCTL, 0x00);

unlock:
	spin_unlock(&nrl->lock);
	return ret;
}

static enum usb_role nuc980_role_sw_get(struct usb_role_switch *sw)
{
	struct nuc980_role_sw *nrl = usb_role_switch_get_drvdata(sw);
	u32 val;

	regmap_read(nrl->regmap, REG_PWRON, &val);
	if (val & PWRON_USBID)
		return USB_ROLE_HOST;

	return USB_ROLE_DEVICE;
}

static int nuc980_role_sw_probe(struct platform_device *pdev)
{
	struct usb_role_switch_desc sw_desc = { };
	struct nuc980_role_sw *nrl;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	int ret;

	dev_set_name(dev, "regmap.nuc980-role-switch");

	nrl = devm_kzalloc(dev, sizeof(*nrl), GFP_KERNEL);
	if (!nrl)
		return -ENOMEM;

	platform_set_drvdata(pdev, nrl);
	nrl->dev = dev;

	nrl->regmap = syscon_regmap_lookup_by_phandle(np, "syscon");
	if (IS_ERR(nrl->regmap)) {
		dev_err(dev, "unable to get syscon region\n");
		return PTR_ERR(nrl->regmap);
	}

	ret = software_node_register(&nuc980_role_sw_node);
	if (ret) {
		dev_err(dev, "unable to register software node\n");
		return ret;
	}

	spin_lock_init(&nrl->lock);

	sw_desc.set = nuc980_role_sw_set;
	sw_desc.get = nuc980_role_sw_get;
	sw_desc.allow_userspace_control = true;
	sw_desc.fwnode = software_node_fwnode(&nuc980_role_sw_node);
	sw_desc.driver_data = nrl;

	nrl->role_sw = usb_role_switch_register(dev, &sw_desc);
	if (IS_ERR(nrl->role_sw)) {
		dev_err(dev, "unable to register role switch\n");
		ret = PTR_ERR(nrl->role_sw);
		goto put_fwnode;
	}

	dev_info(dev, "initialized\n");

	return 0;

put_fwnode:
	software_node_unregister(&nuc980_role_sw_node);
	return ret;
}

static const struct of_device_id nuc980_role_sw_dt_ids[] = {
	{ .compatible = "nuvoton,nuc980-role-sw" },
	{},
};
MODULE_DEVICE_TABLE(of, nuc980_role_sw_dt_ids);

static struct platform_driver nuc980_role_sw_driver = {
	.probe = nuc980_role_sw_probe,
	.driver = {
		.name = "nuc980-role-sw",
		.of_match_table = nuc980_role_sw_dt_ids,
	},
};

static int __init nuc980_role_sw_init(void)
{
	return platform_driver_register(&nuc980_role_sw_driver);
}
device_initcall(nuc980_role_sw_init);
