// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020, Veiko Rütter <rebane@alkohol.ee>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>

#define NUC980_CLK_REG_SIZE 0x100

struct nuc980_clk_item {
	struct clk_hw          *clk_hw;
	u32                    reg;
	u8                     bit;
	bool                   alt;
	struct nuc980_clk_item *next;
};

struct nuc980_clk_parents {
	const char *parent[4];
	u32        table[4];
	int        num;
};

struct nuc980_clk {
	const char   *name;
	void __iomem *base;
	spinlock_t   lock;
	struct nuc980_clk_item *nclki_first;
	struct nuc980_clk_item *nclki_last;
};

struct nuc980_clk_pll {
	struct clk_hw        clk_hw;
	struct clk_init_data init;
	const char           *parent_name;
	void __iomem         *reg;
	struct nuc980_clk    *nclk;
};

static int nuc980_clk_add(struct nuc980_clk *nclk, char *name,
			struct clk_hw *clk_hw, u32 reg, int bit, bool alt)
{
	struct nuc980_clk_item *nclki;

	if (IS_ERR(clk_hw))
		return PTR_ERR(clk_hw);

	nclki = kzalloc(sizeof(*nclki), GFP_KERNEL);
	if (!nclki) {
		pr_err("nuc980-clk %s: unable to add clock: %s\n", nclk->name,
						name);
		return -ENOMEM;
	}

	nclki->clk_hw = clk_hw;
	nclki->reg = reg;
	nclki->bit = bit;
	nclki->alt = alt;
	nclki->next = NULL;

	if (!nclk->nclki_first)
		nclk->nclki_first = nclki;
	else
		nclk->nclki_last->next = nclki;

	nclk->nclki_last = nclki;

	return 0;
}

static struct clk_hw *nuc980_clk_get(struct of_phandle_args *clkspec, void *d)
{
	struct nuc980_clk *nclk = d;
	struct nuc980_clk_item *nclki;
	bool alt = false;

	if (clkspec->args_count < 3)
		return ERR_PTR(-EINVAL);

	if (clkspec->args[2] == 1)
		alt = true;

	for (nclki = nclk->nclki_first; nclki; nclki = nclki->next) {
		if (nclki->reg == clkspec->args[0] &&
				nclki->bit == clkspec->args[1] &&
				nclki->alt == alt) {
			return nclki->clk_hw;
		}
	}

	return ERR_PTR(-EINVAL);
}

static int nuc980_clk_pll_enable(struct clk_hw *clk_hw)
{
	struct nuc980_clk_pll *pll =
			container_of(clk_hw, struct nuc980_clk_pll, clk_hw);
	u32 val = readl(pll->reg);

	val &= ~0x10000000;
	val |= 0x40000000;
	writel(val, pll->reg);

	return 0;
}

static void nuc980_clk_pll_disable(struct clk_hw *clk_hw)
{
	struct nuc980_clk_pll *pll =
			container_of(clk_hw, struct nuc980_clk_pll, clk_hw);
	u32 val = readl(pll->reg);

	val |= 0x10000000;
	val &= ~0x40000000;
	writel(val, pll->reg);
}

static int nuc980_clk_pll_set_rate(struct clk_hw *clk_hw, unsigned long rate,
						unsigned long parent_rate)
{
	struct nuc980_clk_pll *pll =
			container_of(clk_hw, struct nuc980_clk_pll, clk_hw);
	u32 val = readl(pll->reg) & ~0x0fffffff;

	switch (rate) {
	case 96000000:  /* 12MHz * 40 / 5 = 96MHz, unused on NUC980 (USB 48MHz
			 * comes from USB PHY 480MHz/10, not from PLL).
			 * Likely a leftover from Nuvoton BSP. */
		val |= 0x8027;
		break;
	case 98400000:  /* i2s: 12MHz * 41 / 5 = 98.4MHz */
		val |= 0x8028;
		break;
	case 169500000: /* i2s: 12MHz * 113 / 8 = 169.5MHz */
		val |= 0x21f0;
		break;
	case 264000000: /* system default: 12MHz * 22 = 264MHz */
		val |= 0x15;
		break;
	case 300000000: /* 12MHz * 25 = 300MHz */
		val |= 0x18;
		break;
	default:
		val |= 0x15;
		break;
	}

	writel(val, pll->reg);

	return 0;
}

static unsigned long nuc980_clk_pll_recalc_rate(struct clk_hw *clk_hw,
					  unsigned long parent_rate)
{
	struct nuc980_clk_pll *pll =
			container_of(clk_hw, struct nuc980_clk_pll, clk_hw);
	u32 val = readl(pll->reg) & 0x0fffffff;

	if (parent_rate != 12000000)
		return 0;

	if (val == 0x18)
		return 300000000;
	if (val == 0x8027)
		return 96000000;
	if (val == 0x8028)
		return 98400000;
	if (val == 0x21f0)
		return 169500000;

	return 264000000;
}

static long nuc980_clk_pll_round_rate(struct clk_hw *clk_hw,
			unsigned long rate, unsigned long *parent_rate)
{
	static const unsigned long supported[] = {
		96000000, 98400000, 169500000, 264000000, 300000000,
	};
	unsigned long best = supported[0];
	unsigned long best_diff = (rate > best) ? rate - best : best - rate;
	int i;

	for (i = 1; i < ARRAY_SIZE(supported); i++) {
		unsigned long diff = (rate > supported[i]) ?
			rate - supported[i] : supported[i] - rate;
		if (diff < best_diff) {
			best = supported[i];
			best_diff = diff;
		}
	}

	return best;
}

static const struct clk_ops nuc980_clk_pll_ops = {
	.enable = nuc980_clk_pll_enable,
	.disable = nuc980_clk_pll_disable,
	.recalc_rate = nuc980_clk_pll_recalc_rate,
	.set_rate = nuc980_clk_pll_set_rate,
	.round_rate = nuc980_clk_pll_round_rate,
};

static struct clk_hw *nuc980_clk_pll_create(struct nuc980_clk *nclk, char *name,
			u32 reg, unsigned long flags)
{
	struct nuc980_clk_pll *pll;
	int ret;

	pll = kzalloc(sizeof(*pll), GFP_KERNEL);
	if (!pll) {
		ret = -ENOMEM;
		goto err;
	}

	pll->nclk              = nclk;
	pll->reg               = pll->nclk->base + reg;
	pll->init.name         = name;
	pll->init.ops          = &nuc980_clk_pll_ops;
	pll->init.flags        = flags;
	pll->parent_name       = "xin";
	pll->init.parent_names = &pll->parent_name;
	pll->init.num_parents  = 1;
	pll->clk_hw.init       = &pll->init;

	ret = clk_hw_register(NULL, &pll->clk_hw);
	if (ret)
		goto err_free;

	return &pll->clk_hw;

err_free:
	kfree(pll);
err:
	pr_err("nuc980-clk %s: unable create clock: %s\n", nclk->name, name);
	return ERR_PTR(ret);
}

static struct nuc980_clk_parents *nuc980_clk_alloc_parents(char *parent0,
				char *parent1, char *parent2, char *parent3)
{
	struct nuc980_clk_parents *nclkp;
	int i;

	nclkp = kzalloc(sizeof(*nclkp), GFP_KERNEL);
	if (!nclkp)
		return NULL;

	nclkp->parent[0] = parent0;
	nclkp->parent[1] = parent1;
	nclkp->parent[2] = parent2;
	nclkp->parent[3] = parent3;

	for (i = 0; i < 4; i++) {
		if (nclkp->parent[i] == NULL)
			continue;

		nclkp->parent[nclkp->num] = nclkp->parent[i];
		nclkp->table[nclkp->num] = i;
		nclkp->num++;
	}

	return nclkp;
}

static struct clk_hw *nuc980_clk_create(struct nuc980_clk *nclk, char *name,
		bool gate_present, u32 gate_reg, u8 gate_bit,
		bool div_present, u32 div_reg, u8 div_shift, u8 div_width,
		bool fac_present, unsigned fac_div,
		bool fix_present, unsigned fix_rate,
		bool mux_present, u32 mux_reg, u8 mux_shift,
		char *parent0, char *parent1, char *parent2, char *parent3,
		unsigned long flags)
{
	struct clk_mux *clk_mux = NULL;
	struct clk_divider *clk_div = NULL;
	struct clk_fixed_factor *clk_fac = NULL;
	struct clk_fixed_rate *clk_fix = NULL;
	struct clk_gate *clk_gate = NULL;
	struct clk_hw *rate_hw = NULL;
	struct clk_hw *clk_hw = NULL;
	const struct clk_ops *rate_ops = NULL;
	struct nuc980_clk_parents *nclkp = NULL;
	int ret;

	nclkp = nuc980_clk_alloc_parents(parent0, parent1, parent2, parent3);
	if (!nclkp) {
		ret = -ENOMEM;
		goto err;
	}

	if (gate_present) {
		clk_gate = kzalloc(sizeof(*clk_gate), GFP_KERNEL);
		if (!clk_gate) {
			ret = -ENOMEM;
			goto err;
		}

		clk_gate->reg = nclk->base + gate_reg;
		clk_gate->bit_idx = gate_bit;
		clk_gate->lock = &nclk->lock;
	}

	if (div_present) {
		clk_div = kzalloc(sizeof(*clk_div), GFP_KERNEL);
		if (!clk_div) {
			ret = -ENOMEM;
			goto err;
		}
		clk_div->reg = nclk->base + div_reg;
		clk_div->shift = div_shift;
		clk_div->width = div_width;
		clk_div->lock = &nclk->lock;
		rate_hw = &clk_div->hw;
		rate_ops = &clk_divider_ops;
	} else if (fac_present) {
		clk_fac = kzalloc(sizeof(*clk_fac), GFP_KERNEL);
		if (!clk_fac) {
			ret = -ENOMEM;
			goto err;
		}
		clk_fac->mult = 1;
		clk_fac->div = fac_div;
		rate_hw = &clk_fac->hw;
		rate_ops = &clk_fixed_factor_ops;
	} else if (fix_present) {
		clk_fix = kzalloc(sizeof(*clk_fix), GFP_KERNEL);
		if (!clk_fix) {
			ret = -ENOMEM;
			goto err;
		}
		clk_fix->fixed_rate = fix_rate;
		clk_fix->fixed_accuracy = 100;
		rate_hw = &clk_fix->hw;
		rate_ops = &clk_fixed_rate_ops;
	}

	if (mux_present) {
		clk_mux = kzalloc(sizeof(*clk_mux), GFP_KERNEL);
		if (!clk_mux) {
			ret = -ENOMEM;
			goto err;
		}
		clk_mux->reg = nclk->base + mux_reg;
		clk_mux->shift = mux_shift;
		clk_mux->mask = 0x03;
		clk_mux->lock = &nclk->lock;
		clk_mux->table = nclkp->table;
	}

	clk_hw = clk_hw_register_composite(NULL, name,
			nclkp->parent, nclkp->num,
			clk_mux ? &clk_mux->hw : NULL, &clk_mux_ops,
			rate_hw, rate_ops,
			clk_gate ? &clk_gate->hw : NULL, &clk_gate_ops, flags);

	if (IS_ERR(clk_hw)) {
		ret = PTR_ERR(clk_hw);
		goto err;
	}

	return clk_hw;

err:
	pr_err("nuc980-clk %s: unable create clock: %s\n", nclk->name, name);
	if (nclkp)
		kfree(nclkp);
	if (clk_gate)
		kfree(clk_gate);
	if (clk_fix)
		kfree(clk_fix);
	if (clk_fac)
		kfree(clk_fac);
	if (clk_div)
		kfree(clk_div);
	if (clk_mux)
		kfree(clk_mux);

	return ERR_PTR(ret);
}

static void nuc980_clk_gate(struct nuc980_clk *nclk, char *name,
				u32 gate_reg, u8 gate_bit,
				char *parent, unsigned long flags)
{
	struct clk_hw *clk_hw;

	clk_hw = nuc980_clk_create(nclk, name,
		true, gate_reg, gate_bit,
		false, 0, 0, 0,
		false, 0,
		false, 0,
		false, 0, 0,
		parent, NULL, NULL, NULL, flags);

	nuc980_clk_add(nclk, name, clk_hw, gate_reg, gate_bit, false);
}

static void nuc980_clk_gate_div(struct nuc980_clk *nclk, char *name,
				u32 gate_reg, u8 gate_bit,
				u32 div_reg, u8 div_shift, u8 div_width,
				char *parent, unsigned long flags)
{
	struct clk_hw *clk_hw;

	clk_hw = nuc980_clk_create(nclk, name,
		true, gate_reg, gate_bit,
		true, div_reg, div_shift, div_width,
		false, 0,
		false, 0,
		false, 0, 0,
		parent, NULL, NULL, NULL, flags);

	nuc980_clk_add(nclk, name, clk_hw, gate_reg, gate_bit, false);
}

static void nuc980_clk_gate_div_mux(struct nuc980_clk *nclk, char *name,
				u32 gate_reg, u8 gate_bit,
				u32 div_reg, u8 div_shift, u8 div_width,
				u32 mux_reg, u8 mux_shift,
				char *parent0, char *parent1,
				char *parent2, char *parent3)
{
	struct clk_hw *clk_hw;

	clk_hw = nuc980_clk_create(nclk, name,
		true, gate_reg, gate_bit,
		true, div_reg, div_shift, div_width,
		false, 0,
		false, 0,
		true, mux_reg, mux_shift,
		parent0, parent1, parent2, parent3,
		0);

	nuc980_clk_add(nclk, name, clk_hw, gate_reg, gate_bit, false);
}

static void nuc980_clk_gate_fac(struct nuc980_clk *nclk, char *name,
				u32 gate_reg, u8 gate_bit,
				unsigned fac_div,
				char *parent, unsigned long flags)
{
	struct clk_hw *clk_hw;

	clk_hw = nuc980_clk_create(nclk, name,
		true, gate_reg, gate_bit,
		false, 0, 0, 0,
		true, fac_div,
		false, 0,
		false, 0, 0,
		parent, NULL, NULL, NULL, flags);

	nuc980_clk_add(nclk, name, clk_hw, gate_reg, gate_bit, false);
}

static void nuc980_clk_gate_fix(struct nuc980_clk *nclk, char *name,
				u32 gate_reg, u8 gate_bit,
				unsigned fix_rate, unsigned long flags)
{
	struct clk_hw *clk_hw;

	clk_hw = nuc980_clk_create(nclk, name,
		true, gate_reg, gate_bit,
		false, 0, 0, 0,
		false, 0,
		true, fix_rate,
		false, 0, 0,
		NULL, NULL, NULL, NULL, flags);

	nuc980_clk_add(nclk, name, clk_hw, gate_reg, gate_bit, false);
}

static void nuc980_clk_div_mux_nolist(struct nuc980_clk *nclk, char *name,
				u32 div_reg, u8 div_shift,
				u8 div_width, u32 mux_reg, u8 mux_shift,
				char *parent0, char *parent1,
				char *parent2, char *parent3,
				unsigned long flags)
{
	nuc980_clk_create(nclk, name,
		false, 0, 0,
		true, div_reg, div_shift, div_width,
		false, 0,
		false, 0,
		true, mux_reg, mux_shift,
		parent0, parent1, parent2, parent3,
		flags);
}

static void nuc980_clk_fac_alt(struct nuc980_clk *nclk, char *name,
				u32 reg, u8 bit,
				unsigned fac_div, char *parent)
{
	struct clk_hw *clk_hw;

	clk_hw = nuc980_clk_create(nclk, name,
		false, 0, 0,
		false, 0, 0, 0,
		true, fac_div,
		false, 0,
		false, 0, 0,
		parent, NULL, NULL, NULL,
		0);

	nuc980_clk_add(nclk, name, clk_hw, reg, bit, true);
}

static void nuc980_clk_phe(struct nuc980_clk *nclk, char *phclk_name,
				u32 gate_reg, u8 gate_bit, char *eclk_name,
				u32 mux_reg, u8 mux_shift, char *phclk_parent,
				char *eclk_parent0, char *eclk_parent1,
				char *eclk_parent2, char *eclk_parent3)
{
	struct clk_hw *phclk_hw;
	struct clk_hw *eclk_hw;

	phclk_hw = nuc980_clk_create(nclk, phclk_name,
		true, gate_reg, gate_bit,
		false, 0, 0, 0,
		false, 0,
		false, 0,
		false, 0, 0,
		phclk_parent, NULL, NULL, NULL,
		0);

	nuc980_clk_add(nclk, phclk_name, phclk_hw, gate_reg, gate_bit, false);

	eclk_hw = nuc980_clk_create(nclk, eclk_name,
		false, 0, 0,
		false, 0, 0, 0,
		false, 0,
		false, 0,
		true, mux_reg, mux_shift,
		eclk_parent0, eclk_parent1, eclk_parent2, eclk_parent3,
		0);

	nuc980_clk_add(nclk, eclk_name, eclk_hw, gate_reg, gate_bit, true);
}

static void nuc980_clk_div_phe(struct nuc980_clk *nclk, char *phclk_name,
				u32 gate_reg, u8 gate_bit, char *eclk_name,
				u32 div_reg, u8 div_shift, u8 div_width,
				u32 mux_reg, u8 mux_shift, char *phclk_parent,
				char *eclk_parent0, char *eclk_parent1,
				char *eclk_parent2, char *eclk_parent3)
{
	struct clk_hw *phclk_hw;
	struct clk_hw *eclk_hw;

	phclk_hw = nuc980_clk_create(nclk, phclk_name,
		true, gate_reg, gate_bit,
		false, 0, 0, 0,
		false, 0,
		false, 0,
		false, 0, 0,
		phclk_parent, NULL, NULL, NULL,
		0);

	nuc980_clk_add(nclk, phclk_name, phclk_hw, gate_reg, gate_bit, false);

	eclk_hw = nuc980_clk_create(nclk, eclk_name,
		false, 0, 0,
		true, div_reg, div_shift, div_width,
		false, 0,
		false, 0,
		true, mux_reg, mux_shift,
		eclk_parent0, eclk_parent1, eclk_parent2, eclk_parent3,
		0);

	nuc980_clk_add(nclk, eclk_name, eclk_hw, gate_reg, gate_bit, true);
}

static void nuc980_clk_div_phe_nomux(struct nuc980_clk *nclk, char *phclk_name,
				u32 gate_reg, u8 gate_bit, char *eclk_name,
				u32 div_reg, u8 div_shift, u8 div_width,
				char *phclk_parent, char *eclk_parent)
{
	struct clk_hw *phclk_hw;
	struct clk_hw *eclk_hw;

	phclk_hw = nuc980_clk_create(nclk, phclk_name,
		true, gate_reg, gate_bit,
		false, 0, 0, 0,
		false, 0,
		false, 0,
		false, 0, 0,
		phclk_parent, NULL, NULL, NULL,
		0);

	nuc980_clk_add(nclk, phclk_name, phclk_hw, gate_reg, gate_bit, false);

	eclk_hw = nuc980_clk_create(nclk, eclk_name,
		false, 0, 0,
		true, div_reg, div_shift, div_width,
		false, 0,
		false, 0,
		false, 0, 0,
		eclk_parent, NULL, NULL, NULL,
		0);

	nuc980_clk_add(nclk, eclk_name, eclk_hw, gate_reg, gate_bit, true);
}

static void __init nuc980_clk_setup(struct device_node *np)
{
	struct nuc980_clk *nclk;
	struct clk_hw *clk_hw;
	struct resource res;
	struct clk *lxt_clk;
	const char *name;
	char *lxt_name = NULL;

	name = np->full_name;

	nclk = kzalloc(sizeof(*nclk), GFP_KERNEL);
	if (!nclk) {
		pr_err("%s: unable to allocate memory\n", name);
		return;
	}

	spin_lock_init(&nclk->lock);

	if (of_address_to_resource(np, 0, &res)) {
		pr_err("%s: unable to get mem region\n", name);
		goto free_nclk;
	}

	if (!res.start || resource_size(&res) < NUC980_CLK_REG_SIZE) {
		pr_err("%s: mem region out of range\n", name);
		goto free_nclk;
	}

	nclk->name = kasprintf(GFP_KERNEL, "%08x.nuc980-clk",
				(unsigned int)res.start) ?: name;
	name = nclk->name;

	if (!request_mem_region(res.start, resource_size(&res), name)) {
		pr_err("nuc980-clk %s: unable to request mem region\n", name);
		goto free_name;
	}

	nclk->base = ioremap(res.start, resource_size(&res));
	if (!nclk->base) {
		pr_err("nuc980-clk %s: unable to map mem base\n", name);
		goto release_mem;
	}

	lxt_clk = of_clk_get_by_name(np, NULL);
	if (IS_ERR(lxt_clk)) {
		pr_warn("nuc980-clk %s: unable to get clock: lxt\n", name);
	} else {
		lxt_name = (char *)__clk_get_name(lxt_clk);
		clk_put(lxt_clk);
	}

	/* aclk */
	clk_hw = nuc980_clk_pll_create(nclk, "aclk", 0x60, 0);
	nuc980_clk_add(nclk, "aclk", clk_hw, 0x60, 28, false);

	/* uclk */
	clk_hw = nuc980_clk_pll_create(nclk, "uclk", 0x64, CLK_IS_CRITICAL);
	nuc980_clk_add(nclk, "uclk", clk_hw, 0x64, 28, false);

	/* 0x10 */
	nuc980_clk_gate_fix(nclk, "xin", 0x00, 0, 12000000, CLK_IS_CRITICAL);
	nuc980_clk_fac_alt(nclk, "xin_512", 0x00, 0, 512, "xin");
	nuc980_clk_div_mux_nolist(nclk, "sys", 0x20, 8, 1, 0x20, 3, 
		"xin", NULL, "aclk", "uclk", CLK_IS_CRITICAL);
	nuc980_clk_gate_div(nclk, "cpu", 0x10, 0, 0x20, 16, 1,
		"sys", CLK_IS_CRITICAL);
	nuc980_clk_gate_fac(nclk, "dram", 0x10, 1, 2, "sys", CLK_IS_CRITICAL);
	nuc980_clk_gate_fac(nclk, "hclk", 0x10, 1, 2, "sys", CLK_IS_CRITICAL);
	nuc980_clk_gate_fac(nclk, "hclk1", 0x10, 2, 2, "sys", 0);
	nuc980_clk_gate_fac(nclk, "hclk3", 0x10, 3, 2, "sys", 0);
	nuc980_clk_gate_fac(nclk, "hclk4", 0x10, 4, 2, "sys", 0);
	nuc980_clk_gate(nclk, "pclk0", 0x10, 5, "hclk1", 0);
		nuc980_clk_fac_alt(nclk, "pclk0_4096", 0x10, 5, 4096, "pclk0");
	nuc980_clk_gate(nclk, "pclk1", 0x10, 6, "hclk1", 0);
		nuc980_clk_fac_alt(nclk, "pclk1_4096", 0x10, 6, 4096, "pclk1");
	nuc980_clk_gate(nclk, "hclk_sdram", 0x10, 8, "hclk", CLK_IS_CRITICAL);
	nuc980_clk_gate(nclk, "hclk_ebi", 0x10, 9, "hclk1", 0);
	nuc980_clk_gate(nclk, "hclk_ddr", 0x10, 10, "sys", CLK_IS_CRITICAL);
	nuc980_clk_gate(nclk, "hclk_gpio", 0x10, 11, "hclk1", 0);
	nuc980_clk_gate(nclk, "pclk_pdma0", 0x10, 12, "hclk1", 0);
	nuc980_clk_gate(nclk, "pclk_pdma1", 0x10, 13, "hclk1", 0);
	nuc980_clk_gate_fac(nclk, "pclk2", 0x10, 14, 2, "hclk1", 0);
		nuc980_clk_fac_alt(nclk, "pclk2_4096", 0x10, 14, 4096, "pclk2");
	nuc980_clk_div_phe_nomux(nclk, "hclk_emac0", 0x10, 16, "emac0_mdclk",
		0x40, 0, 8, "hclk4", "hclk4");
	nuc980_clk_div_phe_nomux(nclk, "hclk_emac1", 0x10, 17, "emac1_mdclk",
		0x40, 0, 8, "hclk3", "hclk3");
	nuc980_clk_gate(nclk, "hclk_usbh", 0x10, 18, "hclk3", 0);
	nuc980_clk_gate(nclk, "hclk_usbd", 0x10, 19, "hclk3", 0);
	nuc980_clk_gate(nclk, "hclk_fmi", 0x10, 20, "hclk3", 0);
	nuc980_clk_div_phe(nclk, "hclk_sd0", 0x10, 22, "eclk_sd0", 0x2c, 8, 8,
		0x2c, 3, "hclk3", "xin", NULL, "aclk", "uclk");
	nuc980_clk_gate(nclk, "hclk_crypto", 0x10, 23, "hclk3", 0);
	nuc980_clk_div_phe(nclk, "hclk_i2s", 0x10, 24, "eclk_i2s", 0x24, 24, 8,
		0x24, 19, "hclk4", "xin", NULL, "aclk", "uclk");
	nuc980_clk_div_phe(nclk, "hclk_sd1", 0x10, 30, "eclk_sd1", 0x44, 8, 8,
		0x44, 3, "hclk4", "xin", NULL, "aclk", "uclk");

	/* 0x18 */
	nuc980_clk_phe(nclk, "pclk_wdt", 0x18, 0, "eclk_wdt", 0x40, 8,
		"pclk2", "xin", "xin_512", "pclk2_4096", lxt_name);
	nuc980_clk_phe(nclk, "pclk_wwdt", 0x18, 1, "eclk_wwdt", 0x40, 10,
		"pclk2", "xin", "xin_512", "pclk2_4096", lxt_name);
	nuc980_clk_gate(nclk, "pclk_rtc", 0x18, 2, "pclk2", 0);
	nuc980_clk_phe(nclk, "pclk_timer0", 0x18, 8, "eclk_timer0", 0x40, 16,
		"pclk0", "xin", "pclk0", "pclk0_4096", lxt_name);
	nuc980_clk_phe(nclk, "pclk_timer1", 0x18, 9, "eclk_timer1", 0x40, 18,
		"pclk0", "xin", "pclk0", "pclk0_4096", lxt_name);
	nuc980_clk_phe(nclk, "pclk_timer2", 0x18, 10, "eclk_timer2", 0x40, 20,
		"pclk1", "xin", "pclk1", "pclk1_4096", lxt_name);
	nuc980_clk_phe(nclk, "pclk_timer3", 0x18, 11, "eclk_timer3", 0x40, 22,
		"pclk1", "xin", "pclk1", "pclk1_4096", lxt_name);
	nuc980_clk_phe(nclk, "pclk_timer4", 0x18, 12, "eclk_timer4", 0x40, 24,
		"pclk0", "xin", "pclk0", "pclk0_4096", lxt_name);
	nuc980_clk_phe(nclk, "pclk_timer5", 0x18, 13, "eclk_timer5", 0x40, 26,
		"pclk0", "xin", "pclk0", "pclk0_4096", lxt_name);
	nuc980_clk_div_phe(nclk, "pclk_uart0", 0x18, 16, "eclk_uart0", 0x30,
		5, 3, 0x30, 3, "pclk0", "xin", lxt_name, "aclk", "uclk");
	nuc980_clk_div_phe(nclk, "pclk_uart1", 0x18, 17, "eclk_uart1", 0x30,
		13, 3, 0x30, 11, "pclk0", "xin", lxt_name, "aclk", "uclk");
	nuc980_clk_div_phe(nclk, "pclk_uart2", 0x18, 18, "eclk_uart2", 0x30,
		21, 3, 0x30, 19, "pclk0", "xin", lxt_name, "aclk", "uclk");
	nuc980_clk_div_phe(nclk, "pclk_uart3", 0x18, 19, "eclk_uart3", 0x30,
		29, 3, 0x30, 27, "pclk0", "xin", lxt_name, "aclk", "uclk");
	nuc980_clk_div_phe(nclk, "pclk_uart4", 0x18, 20, "eclk_uart4", 0x34,
		5, 3, 0x34, 3, "pclk0", "xin", lxt_name, "aclk", "uclk");
	nuc980_clk_div_phe(nclk, "pclk_uart5", 0x18, 21, "eclk_uart5", 0x34,
		13, 3, 0x34, 11, "pclk0", "xin", lxt_name, "aclk", "uclk");
	nuc980_clk_div_phe(nclk, "pclk_uart6", 0x18, 22, "eclk_uart6", 0x34,
		21, 3, 0x34, 19, "pclk0", "xin", lxt_name, "aclk", "uclk");
	nuc980_clk_div_phe(nclk, "pclk_uart7", 0x18, 23, "eclk_uart7", 0x34,
		29, 3, 0x34, 27, "pclk0", "xin", lxt_name, "aclk", "uclk");
	nuc980_clk_div_phe(nclk, "pclk_uart8", 0x18, 24, "eclk_uart8", 0x38,
		5, 3, 0x38, 3, "pclk0", "xin", lxt_name, "aclk", "uclk");
	nuc980_clk_div_phe(nclk, "pclk_uart9", 0x18, 25, "eclk_uart9", 0x38,
		13, 3, 0x38, 11, "pclk0", "xin", lxt_name, "aclk", "uclk");

	/* 0x1c */
	nuc980_clk_gate(nclk, "pclk_i2c0", 0x1c, 0, "pclk0", 0);
	nuc980_clk_gate(nclk, "pclk_i2c1", 0x1c, 1, "pclk1", 0);
	nuc980_clk_gate(nclk, "pclk_i2c2", 0x1c, 2, "pclk0", 0);
	nuc980_clk_gate(nclk, "pclk_i2c3", 0x1c, 3, "pclk1", 0);
	nuc980_clk_phe(nclk, "pclk_qspi", 0x1c, 4, "eclk_qspi", 0x28, 8,
		"pclk0", "xin", "pclk0", "aclk", "uclk");
	nuc980_clk_phe(nclk, "pclk_spi0", 0x1c, 5, "eclk_spi0", 0x28, 10,
		"pclk1", "xin", "pclk1", "aclk", "uclk");
	nuc980_clk_phe(nclk, "pclk_spi1", 0x1c, 6, "eclk_spi1", 0x28, 12,
		"pclk0", "xin", "pclk0", "aclk", "uclk");
	nuc980_clk_gate_div_mux(nclk, "pclk_adc", 0x1c, 24, 0x3c, 24, 8,
		0x3c, 19, "xin", NULL, "aclk", "uclk");
	nuc980_clk_gate(nclk, "pclk_can0", 0x1c, 8, "pclk2", 0);
	nuc980_clk_gate(nclk, "pclk_can1", 0x1c, 9, "pclk2", 0);
	nuc980_clk_gate(nclk, "pclk_can2", 0x1c, 10, "pclk2", 0);
	nuc980_clk_gate(nclk, "pclk_can3", 0x1c, 11, "pclk2", 0);
	nuc980_clk_div_phe_nomux(nclk, "pclk_smc0", 0x1c, 12, "eclk_smc0",
		0x38, 24, 4, "pclk2", "xin");
	nuc980_clk_div_phe_nomux(nclk, "pclk_smc1", 0x1c, 13, "eclk_smc1",
		0x38, 28, 4, "pclk2", "xin");

	if (of_clk_add_hw_provider(np, nuc980_clk_get, nclk))
		pr_warn("nuc980-clk %s: unable to add clock provider\n", name);

	pr_info("nuc980-clk %s: initialized\n", name);

	return;

release_mem:
	release_mem_region(res.start, resource_size(&res));
free_name:
	if (nclk->name != np->full_name)
		kfree(nclk->name);
free_nclk:
	kfree(nclk);
}

CLK_OF_DECLARE_DRIVER(nuc980_clk, "nuvoton,nuc980-clk", nuc980_clk_setup);
