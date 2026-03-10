// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020, Veiko Rütter <rebane@alkohol.ee>
 *
 * Based on previous work from:
 * Copyright (C) 2018 Nuvoton Technology Corp.
 *
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/spi/spi.h>

struct nuc980_dma {
	void __iomem          *base;
	struct clk            *pclk;
};

static int nuc980_dma_probe(struct platform_device *pdev)
{
	return -ENOMEM;
}

static const struct of_device_id nuc980_dma_dt_ids[] = {
	{ .compatible = "nuvoton,nuc980-dma" },
	{},
};
MODULE_DEVICE_TABLE(of, nuc980_dma_dt_ids);

static struct platform_driver nuc980_dma_driver = {
	.probe = nuc980_dma_probe,
	.driver = {
		.name = "nuc980-dma",
		.of_match_table = nuc980_dma_dt_ids,
	},
};

static int __init nuc980_dma_init(void)
{
	return platform_driver_register(&nuc980_dma_driver);
}
subsys_initcall(nuc980_dma_init);
