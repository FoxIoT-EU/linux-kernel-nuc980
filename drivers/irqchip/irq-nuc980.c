// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020, Veiko Rütter <rebane@alkohol.ee>
 *
 * Based on previous work from:
 * Copyright (C) 2018 Nuvoton Technology Corp.
 * Copyright 2016 Wan Zongshun <mcuos.com@gmail.com>
 *
 */

#include <asm/exception.h>
#include <asm/hardirq.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/reset.h>

#define NUC980_AIC_MIN_IRQ      3
#define NUC980_AIC_MAX_IRQ      63

#define NUC980_AIC_MIN_PRIORITY 1
#define NUC980_AIC_MAX_PRIORITY 7

#define REG_SRCCTL0             0x000
#define REG_IRQNUM              0x120
#define REG_INTEN0              0x130
#define REG_INTEN1              0x134
#define REG_INTDIS0             0x138
#define REG_INTDIS1             0x13C
#define REG_EOIS                0x150
#define NUC980_AIC_REG_SIZE     0x200

static DEFINE_RAW_SPINLOCK(nuc980_aic_lock);
static void __iomem *nuc980_aic_base;
static struct irq_domain *nuc980_aic_domain;

static void nuc980_aic_irq_mask(struct irq_data *d)
{
	if (d->hwirq < 32)
		writel(1 << (d->hwirq), nuc980_aic_base + REG_INTDIS0);
	else
		writel(1 << (d->hwirq - 32), nuc980_aic_base + REG_INTDIS1);
}

static void nuc980_aic_irq_ack(struct irq_data *d)
{
	writel(0x01, nuc980_aic_base + REG_EOIS);
}

static void nuc980_aic_irq_unmask(struct irq_data *d)
{
	if (d->hwirq < 32)
		writel(1 << (d->hwirq), nuc980_aic_base + REG_INTEN0);
	else
		writel(1 << (d->hwirq - 32), nuc980_aic_base + REG_INTEN1);
}

static struct irq_chip nuc980_aic_irq_chip = {
	.irq_ack    = nuc980_aic_irq_ack,
	.irq_mask   = nuc980_aic_irq_mask,
	.irq_unmask = nuc980_aic_irq_unmask,
};

static asmlinkage void __exception_irq_entry
nuc980_aic_handle_irq(struct pt_regs *regs)
{
	u32 hwirq;

	hwirq = readl(nuc980_aic_base + REG_IRQNUM);
	if (!hwirq)
		writel(0x01, nuc980_aic_base + REG_EOIS);

	handle_IRQ(irq_find_mapping(nuc980_aic_domain, hwirq), regs);
}

static int nuc980_aic_domain_map(struct irq_domain *d, unsigned int irq,
			      irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &nuc980_aic_irq_chip, handle_level_irq);
	irq_clear_status_flags(irq, IRQ_NOREQUEST);

	return 0;
}

static int nuc980_aic_domain_xlate(struct irq_domain *d,
				struct device_node *np,
				const u32 *intspec, unsigned int intsize,
				irq_hw_number_t *out_hwirq,
				unsigned int *out_type)
{
	void __iomem *reg;
	int shift;

	if (intsize < 2)
		return -EINVAL;

	if (intspec[0] < NUC980_AIC_MIN_IRQ ||
			intspec[0] > NUC980_AIC_MAX_IRQ)
		return -EINVAL;

	if (intspec[1] < NUC980_AIC_MIN_PRIORITY ||
			intspec[1] > NUC980_AIC_MAX_PRIORITY)
		return -EINVAL;

	*out_hwirq = intspec[0];
	*out_type = IRQ_TYPE_NONE;
	reg = nuc980_aic_base + REG_SRCCTL0 + (*out_hwirq & 0xFFFFFFFC);
	shift = (*out_hwirq & 0x03) << 3;

	raw_spin_lock(&nuc980_aic_lock);
	writel((readl(reg) & ~(0x07 << shift)) | (intspec[1] << shift), reg);
	raw_spin_unlock(&nuc980_aic_lock);

	return 0;
}

static struct irq_domain_ops nuc980_aic_domain_ops = {
	.map = nuc980_aic_domain_map,
	.xlate = nuc980_aic_domain_xlate,
};

static int __init nuc980_aic_of_init(struct device_node *np,
			      struct device_node *parent)
{
	struct resource res;
	struct reset_control *rst;
	const char *name;

	if (nuc980_aic_domain)
		return -EEXIST;

	name = np->full_name;

	if (of_address_to_resource(np, 0, &res))
		panic("%s: unable to get mem region\n", name);

	if (!res.start || resource_size(&res) < NUC980_AIC_REG_SIZE)
		panic("%s: mem region out of range\n", name);

	name = kasprintf(GFP_KERNEL, "%08x.nuc980-aic",
				(unsigned int)res.start) ?: name;

	nuc980_aic_irq_chip.name = name;

	if (!request_mem_region(res.start, resource_size(&res), name))
		panic("nuc980-aic %s: unable to request mem region\n", name);

	rst = of_reset_control_get_shared(np, NULL);
	if (IS_ERR(rst))
		rst = NULL;

	reset_control_deassert(rst);

	nuc980_aic_base = ioremap(res.start, resource_size(&res));
	if (!nuc980_aic_base)
		panic("nuc980-aic %s: unable to map mem region\n", name);

	writel(0xFFFFFFFC, nuc980_aic_base + REG_INTDIS0);
	writel(0xFFFFFFFF, nuc980_aic_base + REG_INTDIS1);

	nuc980_aic_domain = irq_domain_add_linear(np, (NUC980_AIC_MAX_IRQ + 1),
						&nuc980_aic_domain_ops, NULL);

	if (!nuc980_aic_domain)
		panic("nuc980-aic %s: unable to create irq domain\n", name);

	set_handle_irq(nuc980_aic_handle_irq);

	pr_info("nuc980-aic %s: initialized\n", name);

	return 0;
}
IRQCHIP_DECLARE(nuc980, "nuvoton,nuc980-aic", nuc980_aic_of_init);
