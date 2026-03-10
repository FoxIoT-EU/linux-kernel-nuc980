// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020, Veiko Rütter <rebane@alkohol.ee>
 *
 * Based on previous work from:
 * Copyright (C) 2018 Nuvoton Technology Corp.
 *
 */

#include <linux/bcd.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/rtc.h>
#include <linux/slab.h>

#define REG_INIT            0x000
#define REG_RWEN            0x004
#define   RWEN_RWENF        (1 << 16)
#define REG_TIME            0x00c
#define REG_CAL             0x010
#define REG_TIMEFMT         0x014
#define   TIMEFMT_24HEN     (1 << 0)
#define REG_WEEKDAY         0x018
#define REG_TALM            0x01c
#define REG_CALM            0x020
#define REG_INTEN           0x028
#define   INTEN_ALARMINTEN  (1 << 0)
#define   INTEN_TICKINTEN   (1 << 1)
#define REG_INTSTS          0x02c
#define   INTSTS_ALARMINTEN (1 << 0)
#define   INTSTS_TICKINTEN  (1 << 1)
#define REG_PWRCTL          0x034
#define REG_SPR           0x040
#define NUC980_RTC_REG_SIZE 0x100

struct nuc980_rtc {
	void __iomem         *base;
	struct rtc_device    *rtcdev;
	struct clk           *clk;
	struct reset_control *rst;
};

struct nuc980_rtc_bcd_time {
	int bcd_sec;
	int bcd_min;
	int bcd_hour;
	int bcd_mday;
	int bcd_mon;
	int bcd_year;
};

static void nuc980_rtc_reg_write(struct nuc980_rtc *nrtc, int reg, int value)
{
	unsigned int write_timeout = 0x400;

	writel(value, nrtc->base + reg);

	// wait rtc register write finish
	while((readl(nrtc->base + REG_INTSTS) & (1 << 31)) && write_timeout--)
		udelay(1);
}

static irqreturn_t nuc980_rtc_irq(int irq, void *dev_id)
{
	struct nuc980_rtc *nrtc = dev_id;
	unsigned long events = 0;
	unsigned int riir;

	riir = readl(nrtc->base + REG_INTSTS);

	if (riir & INTSTS_ALARMINTEN) {
		nuc980_rtc_reg_write(nrtc, REG_INTSTS, INTSTS_ALARMINTEN);
		events |= RTC_AF | RTC_IRQF;
	}

	if (riir & INTSTS_TICKINTEN) {
		nuc980_rtc_reg_write(nrtc, REG_INTSTS, INTSTS_TICKINTEN);
		events |= RTC_UF | RTC_IRQF;
	}

	rtc_update_irq(nrtc->rtcdev, 1, events);

	return IRQ_HANDLED;
}

static int nuc980_rtc_access_enable(struct nuc980_rtc *nrtc)
{
	unsigned int timeout = 0x800;

	nuc980_rtc_reg_write(nrtc, REG_INIT, 0xa5eb1357);

	mdelay(10);

	nuc980_rtc_reg_write(nrtc, REG_RWEN, 0x0000a965);

	while (!(readl(nrtc->base + REG_RWEN) & RWEN_RWENF) && timeout--)
		mdelay(1);

	if (!timeout)
		return -EPERM;

	return 0;
}

static int nuc980_rtc_bcd2bin(unsigned int timereg, unsigned int calreg,
				unsigned int wdayreg, struct rtc_time *tm)
{
	tm->tm_mday = bcd2bin(calreg >> 0);
	tm->tm_mon  = bcd2bin(calreg >> 8);
	tm->tm_mon  = tm->tm_mon - 1;

	tm->tm_year = bcd2bin(calreg >> 16) + 100;

	tm->tm_sec  = bcd2bin(timereg >> 0);
	tm->tm_min  = bcd2bin(timereg >> 8);
	tm->tm_hour = bcd2bin(timereg >> 16);

	tm->tm_wday = wdayreg;

	return rtc_valid_tm(tm);
}

static int nuc980_rtc_alarm_bcd2bin(unsigned int timereg, unsigned int calreg,
				struct rtc_time *tm)
{
	tm->tm_mday = bcd2bin(calreg >> 0);
	tm->tm_mon  = bcd2bin(calreg >> 8);
	tm->tm_mon  = tm->tm_mon - 1;

	tm->tm_year = bcd2bin(calreg >> 16) + 100;

	tm->tm_sec  = bcd2bin(timereg >> 0);
	tm->tm_min  = bcd2bin(timereg >> 8);
	tm->tm_hour = bcd2bin(timereg >> 16);

	return rtc_valid_tm(tm);
}

static void nuc980_rtc_bin2bcd(struct rtc_time *tm,
				struct nuc980_rtc_bcd_time *bcd)
{
	bcd->bcd_mday = bin2bcd(tm->tm_mday) << 0;
	bcd->bcd_mon  = bin2bcd(tm->tm_mon + 1) << 8;

	if (tm->tm_year < 100) {
		bcd->bcd_year = bin2bcd(tm->tm_year) << 16;
	} else {
		bcd->bcd_year = bin2bcd(tm->tm_year - 100) << 16;
	}

	bcd->bcd_sec  = bin2bcd(tm->tm_sec) << 0;
	bcd->bcd_min  = bin2bcd(tm->tm_min) << 8;
	bcd->bcd_hour = bin2bcd(tm->tm_hour) << 16;
}

static int nuc980_rtc_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	struct nuc980_rtc *nrtc = dev_get_drvdata(dev);

	if (enabled)
		nuc980_rtc_reg_write(nrtc, REG_INTEN,
			readl(nrtc->base + REG_INTEN) | INTEN_ALARMINTEN);
	else
		nuc980_rtc_reg_write(nrtc, REG_INTEN,
			readl(nrtc->base + REG_INTEN) & ~INTEN_ALARMINTEN);
	return 0;
}

static int nuc980_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct nuc980_rtc *nrtc = dev_get_drvdata(dev);
	unsigned int timeval;
	unsigned int clrval;
	unsigned int wdayval;

	timeval = readl(nrtc->base + REG_TIME);
	clrval  = readl(nrtc->base + REG_CAL);
	wdayval = readl(nrtc->base + REG_WEEKDAY);

	return nuc980_rtc_bcd2bin(timeval, clrval, wdayval, tm);
}

static int nuc980_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct nuc980_rtc *nrtc = dev_get_drvdata(dev);
	struct nuc980_rtc_bcd_time bcd;
	unsigned long val;
	int ret;

	nuc980_rtc_bin2bcd(tm, &bcd);

	ret = nuc980_rtc_access_enable(nrtc);
	if (ret)
		return ret;

	val = bcd.bcd_mday | bcd.bcd_mon | bcd.bcd_year;
	nuc980_rtc_reg_write(nrtc, REG_CAL, val);

	val = bcd.bcd_sec | bcd.bcd_min | bcd.bcd_hour;
	nuc980_rtc_reg_write(nrtc, REG_TIME, val);

	val = tm->tm_wday;
	nuc980_rtc_reg_write(nrtc, REG_WEEKDAY, val);

	return 0;
}

static int nuc980_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct nuc980_rtc *nrtc = dev_get_drvdata(dev);
	unsigned int timeval;
	unsigned int carval;

	timeval = readl(nrtc->base + REG_TALM);
	carval  = readl(nrtc->base + REG_CALM);

	return nuc980_rtc_alarm_bcd2bin(timeval, carval, &alrm->time);
}

static int nuc980_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct nuc980_rtc *nrtc = dev_get_drvdata(dev);
	struct nuc980_rtc_bcd_time bcd;
	unsigned long val;
	int ret;

	nuc980_rtc_bin2bcd(&alrm->time, &bcd);

	ret = nuc980_rtc_access_enable(nrtc);
	if (ret)
		return ret;

	val = bcd.bcd_mday | bcd.bcd_mon | bcd.bcd_year;
	val |= (1 << 31); // mask alarm week day
	nuc980_rtc_reg_write(nrtc, REG_CALM, val);

	val = bcd.bcd_sec | bcd.bcd_min | bcd.bcd_hour;
	nuc980_rtc_reg_write(nrtc, REG_TALM, val);

	nuc980_rtc_reg_write(nrtc, REG_PWRCTL,
			readl(nrtc->base + REG_PWRCTL) | (1 << 3));

	return 0;
}

static int nuc980_rtc_nvram_read(void *priv, unsigned int offset, void *val,
				size_t bytes)
{
	struct nuc980_rtc *nrtc = priv;
	unsigned int i;
	u32 *reg = val;
	int ret;

	ret = nuc980_rtc_access_enable(nrtc);
	if (ret)
		return ret;

	for (i = 0; i < bytes / 4; i++) {
		reg[i] = readl(nrtc->base + REG_SPR + offset + (i * 4));
	}

	return 0;
}

static int nuc980_rtc_nvram_write(void *priv, unsigned int offset, void *val,
				size_t bytes)
{
	struct nuc980_rtc *nrtc = priv;
	unsigned int i;
	u32 *reg = val;
	int ret;

	ret = nuc980_rtc_access_enable(nrtc);
	if (ret)
		return ret;

	for (i = 0; i < bytes / 4; i++) {
		nuc980_rtc_reg_write(nrtc, REG_SPR + offset + (i * 4), reg[i]);
	}

	return 0;
}

static struct rtc_class_ops nuc980_rtc_ops = {
	.read_time = nuc980_rtc_read_time,
	.set_time = nuc980_rtc_set_time,
	.read_alarm = nuc980_rtc_read_alarm,
	.set_alarm = nuc980_rtc_set_alarm,
	.alarm_irq_enable = nuc980_rtc_alarm_irq_enable,
};

static int nuc980_rtc_probe(struct platform_device *pdev)
{
	struct nuc980_rtc *nrtc;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct nvmem_config nvmem_cfg = {};
	struct resource res;
	int irq;
	int ret;

	nrtc = devm_kzalloc(dev, sizeof(*nrtc), GFP_KERNEL);
	if (!nrtc) {
		dev_err(dev, "unable to allocate memory\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, nrtc);

	ret = of_address_to_resource(np, 0, &res);
	if (ret) {
		dev_err(dev, "unable to get mem region\n");
		return ret;
	}

	if (!res.start || resource_size(&res) < NUC980_RTC_REG_SIZE) {
		dev_err(dev, "mem region out of range\n");
		return -EINVAL;
	}

	dev_set_name(dev, "%08x.nuc980-rtc", (unsigned int)res.start);

	nrtc->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(nrtc->clk)) {
		dev_warn(dev, "unable to get clock\n");
		nrtc->clk = NULL;
	}

	ret = clk_prepare_enable(nrtc->clk);
	if (ret) {
		dev_err(dev, "unable to enable clock\n");
		return -EINVAL;
	}

	nrtc->rst = devm_reset_control_get_optional_shared(dev, NULL);
	if (!IS_ERR(nrtc->rst))
		reset_control_deassert(nrtc->rst);

	nrtc->base = devm_ioremap_resource(dev, &res);
	if (IS_ERR(nrtc->base)) {
		dev_err(dev, "unable to map mem region\n");
                ret = PTR_ERR(nrtc->base);
		goto disable_clock;
	}

	nrtc->rtcdev = devm_rtc_device_register(dev, dev_name(dev),
						&nuc980_rtc_ops, THIS_MODULE);

	if (IS_ERR(nrtc->rtcdev)) {
		dev_err(dev, "unable to register rtc device\n");
		ret = PTR_ERR(nrtc->rtcdev);
		goto disable_clock;
	}

	nuc980_rtc_reg_write(nrtc, REG_TIMEFMT,
			readl(nrtc->base + REG_TIMEFMT) | TIMEFMT_24HEN);

	irq = irq_of_parse_and_map(np, 0);
	if (irq <= 0) {
		dev_err(dev, "unable to get irq\n");
		ret = -EINVAL;
		goto disable_clock;
	}

	ret = devm_request_irq(dev, irq, nuc980_rtc_irq,
					0, dev_name(dev), nrtc);

	if (ret) {
		dev_err(dev, "unable to request irq\n");
		goto disable_clock;
	}

	nuc980_rtc_reg_write(nrtc, REG_INTEN,
			readl(nrtc->base + REG_INTEN) | INTEN_TICKINTEN);

	nvmem_cfg.name = "nuc980-rtc-nvmem";
	nvmem_cfg.type = NVMEM_TYPE_BATTERY_BACKED;
	nvmem_cfg.word_size = 4;
	nvmem_cfg.stride = 4;
	nvmem_cfg.size = 64;
	nvmem_cfg.reg_read = nuc980_rtc_nvram_read;
	nvmem_cfg.reg_write = nuc980_rtc_nvram_write;
	nvmem_cfg.priv = nrtc;

	ret = devm_rtc_nvmem_register(nrtc->rtcdev, &nvmem_cfg);
	if (ret)
		dev_warn(dev, "unable to register nvmem\n");

	dev_info(dev, "initialized\n");

	return 0;

disable_clock:
	clk_disable_unprepare(nrtc->clk);
	return ret;
}

static const struct of_device_id nuc980_rtc_dt_ids[] = {
	{ .compatible = "nuvoton,nuc980-rtc"},
	{},
};
MODULE_DEVICE_TABLE(of, nuc980_rtc_dt_ids);

static struct platform_driver nuc980_rtc_driver = {
	.probe = nuc980_rtc_probe,
	.driver = {
		.name           = "nuc980-rtc",
		.of_match_table = nuc980_rtc_dt_ids,
	},
};

static int __init nuc980_rtc_init(void)
{
	return platform_driver_register(&nuc980_rtc_driver);
}
device_initcall(nuc980_rtc_init);
