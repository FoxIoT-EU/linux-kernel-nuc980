// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020, Veiko Rütter <rebane@alkohol.ee>
 *
 * Based on previous work from:
 * Copyright (C) 2018 Nuvoton Technology Corp.
 * Copyright 2016 Wan Zongshun <mcuos.com@gmail.com>
 *
 */

#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/reset.h>
#include <linux/sched_clock.h>
#include <linux/types.h>

#define NUC980_TIMER_PRESCALER 99  /* divider = prescaler + 1 */

#define REG_CTL                0x000
#define   CTL_ONESHOT          0
#define   CTL_COUNTEN          BIT(0)
#define   CTL_PERIODIC         BIT(4)
#define REG_PRECNT             0x004
#define REG_CMPR               0x008
#define REG_IER                0x00C
#define REG_ISR                0x010
#define REG_DR                 0x014
#define REG_TCAP               0x018
#define REG_ECTL               0x020
#define NUC980_TIMER_REG_SIZE  0x100

struct nuc980_timer {
	void __iomem *base;
	unsigned     load;
	struct       clock_event_device clkevt;
};

static int nuc980_timer_clockevent_shutdown(struct clock_event_device *clkevt)
{
	struct nuc980_timer *timer =
			container_of(clkevt, struct nuc980_timer, clkevt);
	writel(0x00, timer->base + REG_CTL);
	return 0;
}

static int nuc980_timer_clockevent_set_periodic(
					struct clock_event_device *clkevt)
{
	struct nuc980_timer *timer =
			container_of(clkevt, struct nuc980_timer, clkevt);
	unsigned int val;

	val = readl(timer->base + REG_CTL) & ~(0x03 << 4);
	writel(timer->load, timer->base + REG_CMPR);
	val |= (CTL_PERIODIC | CTL_COUNTEN);
	writel(val, timer->base + REG_CTL);
	return 0;
}

static int nuc980_timer_clockevent_set_oneshot(
					struct clock_event_device *clkevt)
{
	struct nuc980_timer *timer =
			container_of(clkevt, struct nuc980_timer, clkevt);
	unsigned int val;

	val = readl(timer->base + REG_CTL) & ~(0x03 << 4);
	val |= (CTL_ONESHOT | CTL_COUNTEN);
	writel(val, timer->base + REG_CTL);
	return 0;
}

static int nuc980_timer_clockevent_setnextevent(unsigned long evt,
					struct clock_event_device *clkevt)
{
	struct nuc980_timer *timer =
			container_of(clkevt, struct nuc980_timer, clkevt);

	unsigned int val;
	int ret;

	writel(0, timer->base + REG_CTL);
	writel(evt, timer->base + REG_CMPR);
	ret = readl_poll_timeout_atomic(timer->base + REG_DR, val, val == 0,
					1, 100);
	if (ret)
		pr_err("nuc980-timer: timeout waiting for counter reset\n");
	writel(readl(timer->base + REG_CTL) | CTL_COUNTEN,
		timer->base + REG_CTL);

	return ret;
}

static irqreturn_t nuc980_timer_interrupt(int irq, void *dev_id)
{
	struct nuc980_timer *ntimer = dev_id;
	struct clock_event_device *clkevt = &ntimer->clkevt;

	writel(0x01, ntimer->base + REG_ISR);

	clkevt->event_handler(&ntimer->clkevt);

	return IRQ_HANDLED;
}

static int nuc980_timer_clockevent_init(struct device_node *np,
					void __iomem *base, const char *name)
{
	struct nuc980_timer *ntimer;
	struct clk *eclk_a;
	struct clk *pclk_a;
	struct reset_control *rst_a;
	unsigned int rate;
	int irq;
	int ret;

	eclk_a = of_clk_get_by_name(np, "eclk-a");
	if (IS_ERR(eclk_a)) {
		pr_err("nuc980-timer %s: unable to get clock: eclk-a\n", name);
		return PTR_ERR(eclk_a);
	}

	pclk_a = of_clk_get_by_name(np, "pclk-a");
	if (IS_ERR(pclk_a)) {
		pr_warn("nuc980-timer %s: unable to get clock: pclk-a\n", name);
		pclk_a = NULL;
	}

	ret = clk_prepare_enable(eclk_a);
	if (ret) {
		pr_err("nuc980-timer %s: unable to enable clock: eclk-a\n", name);
		goto put_clocks;
	}

	ret = clk_prepare_enable(pclk_a);
	if (ret) {
		pr_err("nuc980-timer %s: unable to enable clock: pclk-a\n", name);
		goto disable_eclk;
	}

	rst_a = of_reset_control_get_shared(np, "rst-a");
	if (IS_ERR(rst_a))
		rst_a = NULL;

	reset_control_deassert(rst_a);

	irq = irq_of_parse_and_map(np, 0);
	if (irq <= 0) {
		pr_err("nuc980-timer %s: unable to get irq\n", name);
		ret = -EINVAL;
		goto put_reset;
	}

	ntimer = kzalloc(sizeof(*ntimer), GFP_KERNEL);
	if (!ntimer) {
		pr_err("nuc980-timer %s: unable to allocate memory\n", name);
		ret = -ENOMEM;
		goto put_reset;
	}

	ntimer->base = base;

	writel(0x00, ntimer->base + REG_CTL);

	ret = request_irq(irq, nuc980_timer_interrupt,
                          IRQF_TIMER | IRQF_IRQPOLL, name,
                          ntimer);
	if (ret) {
		pr_err("nuc980-timer %s: unable to setup irq\n", name);
		goto free_timer;
	}

	rate = clk_get_rate(eclk_a);

	ntimer->load = (rate / HZ);

	writel(0x01, ntimer->base + REG_ISR);
	writel(0x00, ntimer->base + REG_PRECNT);
	writel(0x01, ntimer->base + REG_IER);

	ntimer->clkevt.shift              = 24;
	ntimer->clkevt.features           = CLOCK_EVT_FEAT_PERIODIC |
						CLOCK_EVT_FEAT_ONESHOT;
	ntimer->clkevt.set_state_shutdown = nuc980_timer_clockevent_shutdown;
	ntimer->clkevt.set_state_periodic = nuc980_timer_clockevent_set_periodic;
	ntimer->clkevt.set_state_oneshot  = nuc980_timer_clockevent_set_oneshot;
	ntimer->clkevt.set_next_event     = nuc980_timer_clockevent_setnextevent;
	ntimer->clkevt.tick_resume        = nuc980_timer_clockevent_shutdown;
	ntimer->clkevt.rating             = 300;
	ntimer->clkevt.name               = name;
	ntimer->clkevt.cpumask            = cpumask_of(0);

	clockevents_config_and_register(&ntimer->clkevt, rate, 12, 0xffffff);

	return 0;

free_timer:
	kfree(ntimer);
put_reset:
	reset_control_put(rst_a);
disable_pclk:
	clk_disable_unprepare(pclk_a);
disable_eclk:
	clk_disable_unprepare(eclk_a);
put_clocks:
	clk_put(pclk_a);
	clk_put(eclk_a);
	return ret;
}

static void __iomem *nuc980_timer_reg_dr;
static u64 nuc980_timer_read_sched_clock(void)
{
	return readl(nuc980_timer_reg_dr);
}

static int nuc980_timer_clocksource_init(struct device_node *np,
					void __iomem *base, const char *name)
{
	struct clk *eclk_b;
	struct clk *pclk_b;
	struct reset_control *rst_b;
	int ret;

	eclk_b = of_clk_get_by_name(np, "eclk-b");
	if (IS_ERR(eclk_b)) {
		pr_err("nuc980-timer %s: unable to get clock: eclk-b\n", name);
		return PTR_ERR(eclk_b);
	}

	pclk_b = of_clk_get_by_name(np, "pclk-b");
	if (IS_ERR(pclk_b)) {
		pr_warn("nuc980-timer %s: unable to get clock: pclk-b\n", name);
		pclk_b = NULL;
	}

	ret = clk_prepare_enable(eclk_b);
	if (ret) {
		pr_err("nuc980-timer %s: unable to enable clock: eclk-b\n", name);
		goto put_clocks;
	}

	ret = clk_prepare_enable(pclk_b);
	if (ret) {
		pr_err("nuc980-timer %s: unable to enable clock: pclk-b\n", name);
		goto disable_eclk;
	}

	rst_b = of_reset_control_get_shared(np, "rst-b");
	if (!IS_ERR(rst_b))
		reset_control_deassert(rst_b);

	writel(0x00, base + REG_CTL);
	writel(0xffffffff, base + REG_CMPR);
	writel(NUC980_TIMER_PRESCALER, base + REG_PRECNT);
	writel(CTL_PERIODIC | CTL_COUNTEN, base + REG_CTL);

	nuc980_timer_reg_dr = base + REG_DR;

	clocksource_mmio_init(nuc980_timer_reg_dr,
		name, 120000, 200, 24, clocksource_mmio_readl_up);

	sched_clock_register(nuc980_timer_read_sched_clock, 24, 120000);

	return 0;

disable_eclk:
	clk_disable_unprepare(eclk_b);
put_clocks:
	clk_put(pclk_b);
	clk_put(eclk_b);
	return ret;
}

static int __init nuc980_timer_init(struct device_node *np)
{
	struct resource res;
	void __iomem *base;
	const char *name;
	int ret;

	name = np->full_name;

	if (of_address_to_resource(np, 0, &res)) {
		pr_err("%s: unable to get mem region\n", name);
		return -EINVAL;
	}

	if (!res.start || resource_size(&res) < (NUC980_TIMER_REG_SIZE * 2)) {
		pr_err("%s: mem region out of range\n", name);
		return -EINVAL;
	}

	name = kasprintf(GFP_KERNEL, "%08x.nuc980-timer",
				(unsigned int)res.start) ?: np->full_name;

	if (!request_mem_region(res.start, resource_size(&res), name)) {
		pr_err("nuc980-timer %s: unable to request mem region\n", name);
		return -EBUSY;
	}

	base = ioremap(res.start, resource_size(&res));
	if (!base) {
		pr_err("nuc980-timer %s: unable to map timer base\n", name);
		ret = -EINVAL;
		goto release_mem;
	}

	ret = nuc980_timer_clocksource_init(np,
			base + NUC980_TIMER_REG_SIZE, name);
	if (ret)
		goto unmap;

	ret = nuc980_timer_clockevent_init(np, base, name);
	if (ret)
		goto unmap;

	pr_info("nuc980-timer %s: initialized\n", name);

	return 0;

unmap:
	iounmap(base);
release_mem:
	release_mem_region(res.start, resource_size(&res));
	return ret;
}
TIMER_OF_DECLARE(nuc980, "nuvoton,nuc980-timer", nuc980_timer_init);
