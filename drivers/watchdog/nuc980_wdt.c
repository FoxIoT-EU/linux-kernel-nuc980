// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020, Veiko Rütter <rebane@alkohol.ee>
 *
 * Based on previous work from:
 * Copyright (C) 2018 Nuvoton Technology Corp.
 *
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/watchdog.h>
#include <linux/types.h>

#define REG_WDT_CTL         0x00
#define   CTL_RSTEN         BIT(1)
#define   CTL_RSTF          BIT(2)
#define   CTL_IF            BIT(3)
#define   CTL_WKEN          BIT(4)
#define   CTL_WKF           BIT(5)
#define   CTL_INTEN         BIT(6)
#define   CTL_WDTEN         BIT(7)
#define   CTL_TOUTSEL       (0x0F << 8)
#define REG_WDT_ALTCTL      0x04
#define REG_WDT_RSTCNT      0x08
#define NUC980_WDT_REG_SIZE 0x10

#define REG_SYSCON_REGWPCTL 0x1fc

struct nuc980_wdt {
	void __iomem           *base;
	struct regmap          *regmap;
	struct clk             *pclk;
	struct clk             *eclk;
	struct watchdog_device wdd;
};

static int nuc980_wdt_syscon_unlock(struct nuc980_wdt *nwdt)
{
	u32 val;
	int i;

	regmap_read(nwdt->regmap, REG_SYSCON_REGWPCTL, &val);
	if (val == 0x01)
		return 0;

	for (i = 0; i < 16; i++) {
		regmap_write(nwdt->regmap, REG_SYSCON_REGWPCTL, 0x59);
		regmap_write(nwdt->regmap, REG_SYSCON_REGWPCTL, 0x16);
		regmap_write(nwdt->regmap, REG_SYSCON_REGWPCTL, 0x88);
		regmap_read(nwdt->regmap, REG_SYSCON_REGWPCTL, &val);
		if (val == 0x01)
			return 0;
	}

	return -EBUSY;
}

static int nuc980_wdt_ping(struct watchdog_device *wdd)
{
	struct nuc980_wdt *nwdt = watchdog_get_drvdata(wdd);

	writel(0x00005AA5, nwdt->base + REG_WDT_RSTCNT);

	return 0;
}


static int nuc980_wdt_start(struct watchdog_device *wdd)
{
	struct nuc980_wdt *nwdt = watchdog_get_drvdata(wdd);
	unsigned int val = CTL_RSTEN | CTL_WDTEN;
	unsigned long flags;

	if(wdd->timeout < 2) {
		val |= 0x5 << 8;
	} else if (wdd->timeout < 8) {
		val |= 0x6 << 8;
	} else {
		val |= 0x7 << 8;
	}

	local_irq_save(flags);
	nuc980_wdt_syscon_unlock(nwdt);
	writel(val, nwdt->base + REG_WDT_CTL);
	regmap_write(nwdt->regmap, REG_SYSCON_REGWPCTL, 0x00);
	local_irq_restore(flags);
	nuc980_wdt_ping(wdd);

	return 0;
}

static int nuc980_wdt_stop(struct watchdog_device *wdd)
{
	struct nuc980_wdt *nwdt = watchdog_get_drvdata(wdd);
	unsigned long flags;

	local_irq_save(flags);
	nuc980_wdt_syscon_unlock(nwdt);
	writel(0, nwdt->base + REG_WDT_CTL);
	regmap_write(nwdt->regmap, REG_SYSCON_REGWPCTL, 0x00);
	local_irq_restore(flags);

	return 0;
}


static int nuc980_wdt_set_timeout(struct watchdog_device *wdd,
							unsigned int timeout)
{
	struct nuc980_wdt *nwdt = watchdog_get_drvdata(wdd);
	unsigned long flags;
	unsigned int val;

	val = readl(nwdt->base + REG_WDT_CTL);
	val &= ~CTL_TOUTSEL;
	if(timeout < 2) {
		val |= 0x5 << 8; // 0.5 sec
	} else if (timeout < 8) {
		val |= 0x6 << 8; // 2 sec
	} else if (timeout < 32) {
		val |= 0x7 << 8; // 8 sec
	} else {
		val |= 0x8 << 8; // 32 sec
	}

	local_irq_save(flags);
	nuc980_wdt_syscon_unlock(nwdt);
	writel(val, nwdt->base + REG_WDT_CTL);
	regmap_write(nwdt->regmap, REG_SYSCON_REGWPCTL, 0x00);
	local_irq_restore(flags);

	return 0;
}

static const struct watchdog_info nuc980_wdt_info = {
	.options  = WDIOF_SETTIMEOUT |
			WDIOF_KEEPALIVEPING |
			WDIOF_MAGICCLOSE,
	.identity = "nuc980 watchdog",
};

static struct watchdog_ops nuc980_wdt_ops = {
	.owner = THIS_MODULE,
	.start = nuc980_wdt_start,
	.stop = nuc980_wdt_stop,
	.ping = nuc980_wdt_ping,
	.set_timeout = nuc980_wdt_set_timeout,
};

static int nuc980_wdt_probe(struct platform_device *pdev)
{
	struct nuc980_wdt *nwdt;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct watchdog_device *wdd;
	struct resource res;
	int ret = 0;

	nwdt = devm_kzalloc(dev, sizeof(struct nuc980_wdt), GFP_KERNEL);
	if (!nwdt) {
		dev_err(dev, "unable to allocate memory\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, nwdt);

	ret = of_address_to_resource(np, 0, &res);
	if (ret) {
		dev_err(dev, "unable to get mem region\n");
		return ret;
	}

	if (!res.start || resource_size(&res) < NUC980_WDT_REG_SIZE) {
		dev_err(dev, "mem region out of range\n");
		ret = -EINVAL;
		return ret;
	}

	dev_set_name(dev, "%08x.nuc980-wdt", (unsigned int)res.start);

	nwdt->regmap = syscon_regmap_lookup_by_phandle(np, "syscon");
	if (IS_ERR(nwdt->regmap)) {
		dev_err(dev, "unable to get syscon region\n");
		return PTR_ERR(nwdt->regmap);
	}

	nwdt->eclk = devm_clk_get(dev, "eclk");
	if (IS_ERR(nwdt->eclk)) {
		dev_err(dev, "unable to get clock: eclk\n");
		ret = PTR_ERR(nwdt->eclk);
		return ret;
	}

	nwdt->pclk = devm_clk_get(dev, "pclk");
	if (IS_ERR(nwdt->pclk)) {
		dev_warn(dev, "unable to get clock: pclk\n");
		nwdt->pclk = NULL;
	}

	ret = clk_prepare_enable(nwdt->eclk);
	if (ret) {
		dev_err(dev, "unable to enable clock: eclk\n");
		return ret;
	}

	ret = clk_prepare_enable(nwdt->pclk);
	if (ret) {
		dev_err(dev, "unable to enable clock: pclk\n");
		goto disable_eclk;
	}

	nwdt->base = devm_ioremap_resource(dev, &res);
	if (IS_ERR(nwdt->base)) {
		dev_err(dev, "unable to map mem region\n");
                ret = PTR_ERR(nwdt->base);
		goto disable_pclk;
	}

	wdd = &nwdt->wdd;

//	wdd->status = WATCHDOG_NOWAYOUT_INIT_STATUS;
	wdd->info = &nuc980_wdt_info;
	wdd->ops = &nuc980_wdt_ops;

	wdd->timeout = 2;      // default time out = 2 sec
	wdd->min_timeout = 1;  // min time out = 0.5 sec
	wdd->max_timeout = 32; // max time out = 32 sec
	wdd->bootstatus = 0;

	watchdog_init_timeout(wdd, 2, dev);
//	watchdog_set_nowayout(wdd, WATCHDOG_NOWAYOUT);
	watchdog_set_drvdata(wdd, nwdt);

	ret = devm_watchdog_register_device(dev, wdd);
	if (ret) {
		dev_err(dev, "unable to register watchdog device\n");
		goto disable_pclk;
	}

	dev_info(dev, "initialized\n");

	return 0;

disable_pclk:
	clk_disable_unprepare(nwdt->pclk);
disable_eclk:
	clk_disable_unprepare(nwdt->eclk);
	return ret;
}

static const struct of_device_id nuc980_wdt_dt_ids[] = {
	{ .compatible = "nuvoton,nuc980-wdt" },
	{},
};
MODULE_DEVICE_TABLE(of, nuc980_wdt_of_match);

static struct platform_driver nuc980_wdt_driver = {
	.probe		= nuc980_wdt_probe,
	.driver		= {
		.name	= "nuc980-wdt",
		.of_match_table = nuc980_wdt_dt_ids,
	},
};

static int __init nuc980_wdt_init(void)
{
        return platform_driver_register(&nuc980_wdt_driver);
}
device_initcall(nuc980_wdt_init);
