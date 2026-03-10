// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020, Veiko Rütter <rebane@alkohol.ee>
 *
 * Based on previous work from:
 * Copyright (C) 2018 Nuvoton Technology Corp.
 *
 */

#include <linux/clk.h>
#include <linux/hw_random.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#define REG_PRNG_CTL        0x08
#define   PRNG_CTL_START    (0x01 << 0)
#define   PRNG_CTL_SEEDRLD  (0x01 << 1)
#define   PRNG_CTL_BUSY     (0x01 << 8)
#define REG_PRNG_SEED       0x0C
#define REG_PRNG_KEY0       0x10
#define NUC980_RNG_REG_SIZE 0x30

struct nuc980_rng {
	struct device *dev;
	void __iomem  *base;
	struct clk    *clk;
	int           rng_data_cnt;
	struct hwrng  rng;
};

#define to_nuc980_rng(p)		\
	container_of((p), struct nuc980_rng, rng)

static int nuc980_rng_read(struct hwrng *rng, void *data, size_t max, bool wait)
{
	struct nuc980_rng *nrng = to_nuc980_rng(rng);
	int count;
	u32 ctl;
	int ret;

	for (count = 0; count < max; count += sizeof(u32)) {
		ctl = readl(nrng->base + REG_PRNG_CTL);
		if ((ctl & PRNG_CTL_BUSY) && wait) {
			ret = readl_poll_timeout_atomic(
					nrng->base + REG_PRNG_CTL,
					ctl, !(ctl & PRNG_CTL_BUSY),
					10, 50000);

			if (ret)
				dev_err(nrng->dev, "%s: timeout %x!\n",
					__func__, ctl);
		}

		if (ctl & PRNG_CTL_BUSY)
			return count ? count : -EIO;

		*(u32 *)data = readl(nrng->base + REG_PRNG_KEY0);
		data += sizeof(u32);
		nrng->rng_data_cnt++;

		if (nrng->rng_data_cnt >= 0x4000000)
		{
			nrng->rng_data_cnt = 0;
			writel(jiffies, nrng->base + REG_PRNG_SEED);
			writel(PRNG_CTL_SEEDRLD, nrng->base + REG_PRNG_CTL);
		}
		writel(PRNG_CTL_START, nrng->base + REG_PRNG_CTL);
	}

	return count;
}

static int nuc980_rng_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct nuc980_rng *nrng;
	struct reset_control *rst;
	struct resource res;
	int ret;

	nrng = devm_kzalloc(dev, sizeof(*nrng), GFP_KERNEL);
	if (!nrng) {
		dev_err(dev, "unable to allocate memory\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, nrng);
	nrng->dev = dev;

	ret = of_address_to_resource(np, 0, &res);
	if (ret) {
		dev_err(dev, "unable to get mem region\n");
		return -EINVAL;
	}

	if (!res.start || resource_size(&res) < NUC980_RNG_REG_SIZE) {
		dev_err(dev, "mem region out of range\n");
		return -EINVAL;
	}

	dev_set_name(dev, "%08x.nuc980-rng", (unsigned int)res.start);

	nrng->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(nrng->clk)) {
		dev_warn(dev, "unable to get clock\n");
		nrng->clk = NULL;
	}

	ret = clk_prepare_enable(nrng->clk);
	if (ret) {
		dev_err(dev, "unable to enable clock\n");
		return ret;
	}

	rst = devm_reset_control_get_optional_shared(dev, NULL);
	if (!IS_ERR(rst))
		reset_control_deassert(rst);

	nrng->base = devm_ioremap_resource(dev, &res);
	if (IS_ERR(nrng->base)) {
		dev_err(dev, "unable to map mem region\n");
                ret = PTR_ERR(nrng->base);
		goto disable_clk;
	}

	nrng->rng_data_cnt  = 0;
	nrng->rng.name      = dev_driver_string(dev),
	nrng->rng.read      = nuc980_rng_read,
	nrng->rng.quality   = 900;

	writel(jiffies, nrng->base + REG_PRNG_SEED);
	writel(PRNG_CTL_SEEDRLD | PRNG_CTL_START, nrng->base + REG_PRNG_CTL);

	ret = devm_hwrng_register(dev, &nrng->rng);
	if (ret) {
		dev_err(dev, "unable to register rng controller\n");
		goto disable_clk;
	}

	dev_info(dev, "initialized\n");

	return 0;

disable_clk:
	clk_disable_unprepare(nrng->clk);
	return ret;
}

static const struct of_device_id nuc980_rng_dt_ids[] = {
	{ .compatible = "nuvoton,nuc980-rng" },
	{},
};
MODULE_DEVICE_TABLE(of, nuc980_rng_dt_ids);

static struct platform_driver nuc980_rng_driver = {
	.probe = nuc980_rng_probe,
	.driver = {
		.name = "nuc980-rng",
		.of_match_table = nuc980_rng_dt_ids,
	},
};

static int __init nuc980_rng_init(void)
{
        return platform_driver_register(&nuc980_rng_driver);
}
device_initcall(nuc980_rng_init);
