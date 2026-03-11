// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020, Veiko Rütter <rebane@alkohol.ee>
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/machine.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/spinlock.h>

#include "../pinctrl-utils.h"

#define REG_MODE              0x00
#define REG_DINOFF            0x04
#define REG_DOUT              0x08
#define REG_DATMSK            0x0C
#define REG_PIN               0x10
#define REG_DBEN              0x14
#define REG_INTTYPE           0x18
#define REG_INTEN             0x1C
#define REG_INTSRC            0x20
#define REG_SMTEN             0x24
#define REG_SLEWCTL           0x28
#define REG_PUSEL             0x30
#define NUC980_PORT_REG_SIZE  0x40

#define SYSCON_OFFSET         0x70

#define CONF_SET              0x80
#define CONF_MASK             0x7F

static const char * const nuc980_pinmux_functions[] = {
	"mfp0", "mfp1", "mfp2", "mfp3",	"mfp4", "mfp5", "mfp6", "mfp7", "mfp8"
};

struct nuc980_pinctrl_gpio {
	const char            *name;
	struct device_node    *np;
	struct nuc980_pinctrl *npctl;
	void __iomem          *base;
	raw_spinlock_t        lock;
	struct gpio_chip      gc;
	struct irq_chip       ic;
	struct irq_domain     *irq_domain;
	int                   irq;
	u32                   inten;
	u8                    out[16];
	int                   pin_idx[16];
	const char            *names[16];
	const char            *groups[16];
	struct lock_class_key gpio_lock_class;
	struct lock_class_key gpio_request_class;
};

struct nuc980_pinctrl_pin {
	unsigned config;
	u8       hwconf_bias;
	u8       hwconf_drive;
	u8       hwconf_debounce;
	u8       hwconf_schmitt;
	u8       hwconf_input;
	u8       hwconf_data;
	u8       hwconf_dir;
	int      hwpin;
};

struct nuc980_pinctrl {
	bool                       initialized;
	struct device              *dev;
	struct device_node         *np;
	struct regmap              *regmap;
	int                        nbanks;
	struct pinctrl_desc        desc;
	struct pinctrl_dev         *pctl;
	const char                 **group_names;
	struct nuc980_pinctrl_pin  *pins;
	struct nuc980_pinctrl_gpio *ngpio;
};

static int nuc980_pinctrl_gpio_get_value(struct gpio_chip *gc,
					unsigned gpio_idx)
{
	struct nuc980_pinctrl_gpio *ngpio = gpiochip_get_data(gc);
	u32 reg_val;

	reg_val = readl(ngpio->base + REG_PIN);
	return !!(reg_val & BIT(gpio_idx));
}

static void nuc980_pinctrl_gpio_reg(struct nuc980_pinctrl_gpio *ngpio,
					u32 reg,
					int gpio_idx,
					int shift,
					unsigned value)
{
	u32 reg_val;
	u32 mask;

	mask = ((1 << shift) - 1) << (gpio_idx * shift);
	reg_val = readl(ngpio->base + reg);
	reg_val &= ~mask;
	reg_val |= value << (gpio_idx * shift);
	writel(reg_val, ngpio->base + reg);
}

static void nuc980_pinctrl_gpio_out(struct nuc980_pinctrl_gpio *ngpio,
					int gpio_idx,
					u8 value,
					bool forced)
{
	if (forced)
		ngpio->out[gpio_idx] = value;

	if (!ngpio->out[gpio_idx])
		ngpio->out[gpio_idx] = 0x01;

	nuc980_pinctrl_gpio_reg(ngpio, REG_MODE, gpio_idx, 2,
						ngpio->out[gpio_idx]);
}

static void nuc980_pinctrl_gpio_set_value(struct gpio_chip *gc,
					unsigned gpio_idx,
					int val)
{
	struct nuc980_pinctrl_gpio *ngpio = gpiochip_get_data(gc);
	unsigned long flags;

	// TODO: there is option to use GPIO_BA + 0x800 addresses for bitbang
	raw_spin_lock_irqsave(&ngpio->lock, flags);
	nuc980_pinctrl_gpio_reg(ngpio, REG_DOUT, gpio_idx, 1, !!val);
	raw_spin_unlock_irqrestore(&ngpio->lock, flags);
}

static int nuc980_pinctrl_gpio_get_direction(struct gpio_chip *gc,
					unsigned gpio_idx)
{
	struct nuc980_pinctrl_gpio *ngpio = gpiochip_get_data(gc);
	u32 reg_val;
	int shift;

	shift = gpio_idx << 1;
	reg_val = readl(ngpio->base + REG_MODE);
	reg_val = (reg_val >> shift) & 0x03;

	return reg_val ? GPIO_LINE_DIRECTION_OUT : GPIO_LINE_DIRECTION_IN;
}

static int nuc980_pinctrl_gpio_direction_input(struct gpio_chip *gc,
					unsigned gpio_idx)
{
	struct nuc980_pinctrl_gpio *ngpio = gpiochip_get_data(gc);
	unsigned long flags;

	raw_spin_lock_irqsave(&ngpio->lock, flags);
	nuc980_pinctrl_gpio_reg(ngpio, REG_MODE, gpio_idx, 2, 0);
	raw_spin_unlock_irqrestore(&ngpio->lock, flags);

	return 0;
}

static int nuc980_pinctrl_gpio_direction_output(struct gpio_chip *gc,
					unsigned gpio_idx,
					int val)
{
	struct nuc980_pinctrl_gpio *ngpio = gpiochip_get_data(gc);
	unsigned long flags;

	raw_spin_lock_irqsave(&ngpio->lock, flags);
	nuc980_pinctrl_gpio_reg(ngpio, REG_DOUT, gpio_idx, 1, !!val);
	nuc980_pinctrl_gpio_out(ngpio, gpio_idx, 0, false);
	raw_spin_unlock_irqrestore(&ngpio->lock, flags);

	return 0;
}

static int nuc980_pinctrl_gpio_set_config(struct gpio_chip *gc,
					unsigned gpio_idx,
					unsigned long config)
{
	struct nuc980_pinctrl_gpio *ngpio = gpiochip_get_data(gc);
	enum pin_config_param param;
	enum pin_config_param arg;
	unsigned long flags;
	int ret = 0;

	param = pinconf_to_config_param(config);
	arg = pinconf_to_config_argument(config);

	raw_spin_lock_irqsave(&ngpio->lock, flags);

	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
	case PIN_CONFIG_BIAS_PULL_PIN_DEFAULT:
	case PIN_CONFIG_BIAS_HIGH_IMPEDANCE:
		nuc980_pinctrl_gpio_reg(ngpio, REG_PUSEL, gpio_idx, 2, 0x00);
		break;

	case PIN_CONFIG_BIAS_PULL_DOWN:
		nuc980_pinctrl_gpio_reg(ngpio, REG_PUSEL, gpio_idx, 2, 0x02);
		break;

	case PIN_CONFIG_BIAS_PULL_UP:
		nuc980_pinctrl_gpio_reg(ngpio, REG_PUSEL, gpio_idx, 2, 0x01);
		break;

	case PIN_CONFIG_DRIVE_OPEN_DRAIN:
		nuc980_pinctrl_gpio_out(ngpio, gpio_idx, 0x02, true);
		break;

	case PIN_CONFIG_DRIVE_PUSH_PULL:
		nuc980_pinctrl_gpio_out(ngpio, gpio_idx, 0x01, true);
		break;

	case PIN_CONFIG_DRIVE_STRENGTH:
		// TODO: only for port B
		break;

	case PIN_CONFIG_DRIVE_STRENGTH_UA:
		// TODO: only for port B
		break;

	case PIN_CONFIG_INPUT_DEBOUNCE:
		nuc980_pinctrl_gpio_reg(ngpio, REG_DBEN, gpio_idx, 1, !!arg);
		break;

	case PIN_CONFIG_INPUT_ENABLE:
		nuc980_pinctrl_gpio_reg(ngpio, REG_DINOFF, gpio_idx + 16, 1, !arg);
		break;

	case PIN_CONFIG_INPUT_SCHMITT_ENABLE:
		nuc980_pinctrl_gpio_reg(ngpio, REG_SMTEN, gpio_idx, 1, !!arg);
		break;

	case PIN_CONFIG_OUTPUT_ENABLE:
		if (arg) {
			nuc980_pinctrl_gpio_out(ngpio, gpio_idx, 0x00, false);
		} else {
			nuc980_pinctrl_gpio_reg(ngpio, REG_MODE, gpio_idx, 2, 0);
		}
		break;

	case PIN_CONFIG_OUTPUT:
		nuc980_pinctrl_gpio_reg(ngpio, REG_DOUT, gpio_idx, 1, !!arg);
		nuc980_pinctrl_gpio_out(ngpio, gpio_idx, 0x00, false);
		break;

	case PIN_CONFIG_SLEW_RATE:
		// TODO
		break;

	default:
		ret = -ENOTSUPP;
	}

	raw_spin_unlock_irqrestore(&ngpio->lock, flags);

	return ret;
}

static void nuc980_pinctrl_gpio_set_multiple(struct gpio_chip *gc,
					unsigned long *mask,
					unsigned long *bits)
{
	struct nuc980_pinctrl_gpio *ngpio = gpiochip_get_data(gc);
	unsigned long flags;

	raw_spin_lock_irqsave(&ngpio->lock, flags);
	writel(~(*mask), ngpio->base + REG_DATMSK);
	writel(*bits, ngpio->base + REG_DOUT);
	writel(0, ngpio->base + REG_DATMSK);
	raw_spin_unlock_irqrestore(&ngpio->lock, flags);
}

static int nuc980_pinctrl_gpio_to_irq(struct gpio_chip *gc, unsigned gpio)
{
	struct nuc980_pinctrl_gpio *ngpio = gpiochip_get_data(gc);

	if (gpio >= ngpio->gc.ngpio)
		return -EINVAL;

	return irq_create_mapping(ngpio->irq_domain, gpio);
}

static void nuc980_pinctrl_gpio_irq_ack(struct irq_data *d)
{
	struct nuc980_pinctrl_gpio *ngpio = irq_data_get_irq_chip_data(d);
	unsigned long flags;

	raw_spin_lock_irqsave(&ngpio->lock, flags);
	writel(BIT(d->hwirq), ngpio->base + REG_INTSRC);
	raw_spin_unlock_irqrestore(&ngpio->lock, flags);
}

static void nuc980_pinctrl_gpio_irq_mask(struct irq_data *d)
{
	struct nuc980_pinctrl_gpio *ngpio = irq_data_get_irq_chip_data(d);
	unsigned long flags;
	u32 val;

	raw_spin_lock_irqsave(&ngpio->lock, flags);
	val = readl(ngpio->base + REG_INTEN);
	val &= ~BIT(d->hwirq);
	val &= ~BIT(d->hwirq + 16);
	writel(val, ngpio->base + REG_INTEN);
	raw_spin_unlock_irqrestore(&ngpio->lock, flags);
}

static void nuc980_pinctrl_gpio_irq_unmask(struct irq_data *d)
{
	struct nuc980_pinctrl_gpio *ngpio = irq_data_get_irq_chip_data(d);
	unsigned long flags;
	u32 val;

	raw_spin_lock_irqsave(&ngpio->lock, flags);
	val = readl(ngpio->base + REG_INTEN);
	val |= (BIT(d->hwirq) & ngpio->inten);
	val |= (BIT(d->hwirq + 16) & ngpio->inten);
	writel(val, ngpio->base + REG_INTEN);
	raw_spin_unlock_irqrestore(&ngpio->lock, flags);
}

static int nuc980_pinctrl_gpio_irq_set_type(struct irq_data *d,
					unsigned type)
{
	struct nuc980_pinctrl_gpio *ngpio = irq_data_get_irq_chip_data(d);
	unsigned long flags;
	bool level = false;
	u32 inten = 0;
	u32 mask;
	u32 val;

	switch (type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_EDGE_FALLING:
		inten |= BIT(d->hwirq);
		break;

	case IRQ_TYPE_EDGE_RISING:
		inten |= BIT(d->hwirq + 16);
		break;

	case IRQ_TYPE_EDGE_BOTH:
		inten |= BIT(d->hwirq);
		inten |= BIT(d->hwirq + 16);
		break;

	case IRQ_TYPE_LEVEL_LOW:
		inten |= BIT(d->hwirq);
		level = true;
		break;

	case IRQ_TYPE_LEVEL_HIGH:
		inten |= BIT(d->hwirq + 16);
		level = true;
		break;

	default:
		return -EINVAL;
	}

	mask = BIT(d->hwirq) | BIT(d->hwirq + 16);

	raw_spin_lock_irqsave(&ngpio->lock, flags);

	val = readl(ngpio->base + REG_INTTYPE);
	if (level)
		val |= BIT(d->hwirq);
	else
		val &= ~BIT(d->hwirq);

	writel(val, ngpio->base + REG_INTTYPE);

	ngpio->inten = (ngpio->inten & ~mask) | inten;

	val = readl(ngpio->base + REG_INTEN);
	if (val & mask) {
		val = (val & ~mask) | ngpio->inten;
		writel(val, ngpio->base + REG_INTEN);
	}

	raw_spin_unlock_irqrestore(&ngpio->lock, flags);

	return 0;
}

static void nuc980_pinctrl_gpio_irq(struct irq_desc *desc)
{
	struct nuc980_pinctrl_gpio *ngpio = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned long sta;
	int virq;
	int bit;
	u32 mask;
	u32 val;

	chained_irq_enter(chip, desc);

	val = readl(ngpio->base + REG_INTEN);
	mask = (val & 0xffff) | (val >> 16);

	while (((sta = readl(ngpio->base + REG_INTSRC)) & mask)) {
		writel(sta, ngpio->base + REG_INTSRC);
		sta &= mask;
		for_each_set_bit(bit, &sta, 32) {
			virq = irq_find_mapping(ngpio->irq_domain, bit);
			generic_handle_irq(virq);
		}
	}

	chained_irq_exit(chip, desc);
}

static int nuc980_pinctrl_gpio_irq_reqres(struct irq_data *d)
{
	struct nuc980_pinctrl_gpio *ngpio = irq_data_get_irq_chip_data(d);

	return gpiochip_reqres_irq(&ngpio->gc, d->hwirq);
}

static void nuc980_pinctrl_gpio_irq_relres(struct irq_data *d)
{
	struct nuc980_pinctrl_gpio *ngpio = irq_data_get_irq_chip_data(d);

	gpiochip_relres_irq(&ngpio->gc, d->hwirq);
}

static int nuc980_pinctrl_gpio_irq_map(struct irq_domain *d,
					unsigned irq,
					irq_hw_number_t hwirq)
{
	struct nuc980_pinctrl_gpio *ngpio = d->host_data;
	int ret;

	ret = irq_set_chip_data(irq, ngpio);
	if (ret)
		return ret;

	irq_set_lockdep_class(irq, &ngpio->gpio_lock_class,
					&ngpio->gpio_request_class);
	irq_set_chip_and_handler(irq, &ngpio->ic, handle_simple_irq);
	irq_set_noprobe(irq);

	return 0;
}

static const struct irq_domain_ops nuc980_pinctrl_gpio_irq_ops = {
	.map = nuc980_pinctrl_gpio_irq_map,
	.xlate = irq_domain_xlate_twocell,
};

static int nuc980_pinctrl_gpio_probe(struct nuc980_pinctrl_gpio *ngpio,
					struct device *dev,
					struct device_node *np,
					struct nuc980_pinctrl *npctl)
{
	struct device_node *pinctrl_np;
	struct property *nuvoton_pinctrl;
	struct resource res;
	struct clk *hclk;
	struct reset_control *rst;
	bool nuvoton_ok = false;
	const char *name;
	u32 ngpios;
	u32 port;
	int irq;
	int ret;
	int i;

	ngpio->gc.base = -1;
	for (i = 0; i < 16; i++) {
		ngpio->pin_idx[i] = -1;
		ngpio->names[i] = NULL;
	}

	raw_spin_lock_init(&ngpio->lock);
	ngpio->np = np;
	ngpio->npctl = npctl;

	ret = of_address_to_resource(np, 0, &res);
	if (ret) {
		dev_err(dev, "unable to get mem region\n");
		return ret;
	}

	if (!res.start || resource_size(&res) < NUC980_PORT_REG_SIZE) {
		dev_err(dev, "mem region out of range\n");
		return -EINVAL;
	}

	ngpio->name = devm_kasprintf(dev, GFP_KERNEL,
				"%08x.nuc980-gpio",
				(unsigned)res.start) ?: dev_name(dev);

	hclk = of_clk_get(np, 0);
	if (IS_ERR(hclk)) {
		pr_warn("nuc980-pinctrl %s: unable to get clock\n",
							ngpio->name);
		hclk = NULL;
	}

	ret = clk_prepare_enable(hclk);
	if (ret) {
		pr_err("nuc980-pinctrl %s: unable to enable clock\n",
							ngpio->name);
		goto put_clock;
	}

	rst = of_reset_control_get_shared(np, NULL);
	if (IS_ERR(rst))
		rst = NULL;

	reset_control_deassert(rst);

	if (!request_mem_region(res.start, resource_size(&res), ngpio->name)) {
		pr_err("nuc980-pinctrl %s: unable to request mem region\n",
							ngpio->name);
		ret = -EBUSY;
		goto disable_clock;
	}

	ngpio->base = ioremap(res.start, resource_size(&res));
	if (!ngpio->base) {
		pr_err("nuc980-pinctrl %s: unable to map mem region\n",
							ngpio->name);
		ret = -EBUSY;
		goto release_mem;
	}

	writel(0, ngpio->base + REG_DATMSK);

	irq = irq_of_parse_and_map(np, 0);
	if (irq <= 0) {
		pr_err("nuc980-pinctrl %s: unable to get irq\n",
							ngpio->name);
		ret = -EINVAL;
		goto unmap_mem;
	}

	if (of_property_read_u32(np, "ngpios", &ngpios))
		ngpios = 16;

	ngpio->gc.ngpio            = min(ngpios, (u32)16);
	ngpio->gc.label            = ngpio->name;
	ngpio->gc.get              = nuc980_pinctrl_gpio_get_value;
	ngpio->gc.set              = nuc980_pinctrl_gpio_set_value;
	ngpio->gc.get_direction    = nuc980_pinctrl_gpio_get_direction;
	ngpio->gc.direction_input  = nuc980_pinctrl_gpio_direction_input;
	ngpio->gc.direction_output = nuc980_pinctrl_gpio_direction_output;
	ngpio->gc.set_config       = nuc980_pinctrl_gpio_set_config;
	ngpio->gc.set_multiple     = nuc980_pinctrl_gpio_set_multiple;
	ngpio->gc.to_irq           = nuc980_pinctrl_gpio_to_irq;
	ngpio->gc.can_sleep        = false;
	ngpio->gc.parent           = dev;
	ngpio->gc.fwnode           = of_fwnode_handle(np);
	ngpio->gc.owner            = THIS_MODULE;

	ngpio->ic.name                  = ngpio->name;
	ngpio->ic.irq_ack               = nuc980_pinctrl_gpio_irq_ack;
	ngpio->ic.irq_mask              = nuc980_pinctrl_gpio_irq_mask;
	ngpio->ic.irq_unmask            = nuc980_pinctrl_gpio_irq_unmask;
	ngpio->ic.irq_set_type          = nuc980_pinctrl_gpio_irq_set_type;
	ngpio->ic.irq_request_resources = nuc980_pinctrl_gpio_irq_reqres;
	ngpio->ic.irq_release_resources = nuc980_pinctrl_gpio_irq_relres;

	ngpio->inten = 0x00;

	/* nuvoton,pinctrl begin */
	ngpios = 0;
	pinctrl_np = of_parse_phandle(np, "nuvoton,pinctrl", 0);
	nuvoton_pinctrl = of_find_property(np, "nuvoton,pinctrl", NULL);
	if (pinctrl_np == npctl->np && nuvoton_pinctrl &&
		((nuvoton_pinctrl->length / sizeof(u32)) >= 2) &&
		!of_property_read_u32_index(np, "nuvoton,pinctrl", 1, &port)){

		nuvoton_ok = true;
		port *= 16;
		for (i = 0; i < npctl->desc.npins; i++) {
			if (npctl->pins[i].hwpin >= port &&
					npctl->pins[i].hwpin < (port + 16)){

				if (!of_property_read_string_index(np,
						"gpio-line-names",
						npctl->pins[i].hwpin - port,
						&name) &&
						strlen(name)) {
					ngpio->names[npctl->pins[i].hwpin - port] =
						name;
				} else {
					ngpio->names[npctl->pins[i].hwpin - port] =
						npctl->desc.pins[i].name;
				}

				ngpio->groups[npctl->pins[i].hwpin - port] =
					npctl->desc.pins[i].name;

				if (((npctl->pins[i].hwpin - port) + 1) > ngpios)
					ngpios = (npctl->pins[i].hwpin - port) + 1;
			}
		}
		if (ngpio->gc.ngpio > ngpios)
			ngpio->gc.ngpio = ngpios;

		ngpio->gc.names = ngpio->names;
	}
	/* nuvoton,pinctrl end */

	ret = devm_gpiochip_add_data(dev, &ngpio->gc, ngpio);
	if (ret) {
		pr_err("nuc980-pinctrl %s: unable to register gpio chip\n",
						ngpio->name);
		goto unmap_mem;
	}

	/* nuvoton,pinctrl begin */
	if (nuvoton_ok && ngpios) {
		for (i = 0; i < ngpio->gc.ngpio; i++) {
			if (ngpio->groups[i])
				gpiochip_add_pingroup_range(&ngpio->gc,
					npctl->pctl, i,
					ngpio->groups[i]);
		}
	}
	/* nuvoton,pinctrl end */

	ngpio->irq_domain = irq_domain_add_linear(np, ngpio->gc.ngpio,
					&nuc980_pinctrl_gpio_irq_ops, ngpio);

	if (!ngpio->irq_domain) {
		pr_err("nuc980-pinctrl %s: unable to create irq domain\n",
					ngpio->name);
		ret = -EINVAL;
		goto unmap_mem;
	}

	irq_set_chained_handler_and_data(irq, nuc980_pinctrl_gpio_irq, ngpio);

	pr_info("nuc980-pinctrl %s: initialized\n", ngpio->name);

	return 0;

unmap_mem:
	iounmap(ngpio->base);
release_mem:
	release_mem_region(res.start, resource_size(&res));
disable_clock:
	clk_disable_unprepare(hclk);

	reset_control_put(rst);
put_clock:
	clk_put(hclk);
	return ret;
}

static struct nuc980_pinctrl_gpio *nuc980_pinctrl_find_gpio_idx(
					struct nuc980_pinctrl *npctl,
					int pin_idx,
					unsigned *gpio_idx)
{
	struct nuc980_pinctrl_gpio *ngpio;
	struct pinctrl_gpio_range *range;
	unsigned gpio;
	int pin;
	int i;

	pin = npctl->desc.pins[pin_idx].number;
	range = pinctrl_find_gpio_range_from_pin(npctl->pctl, pin);

	if (!range)
		return NULL;

	gpio = range->base + range->pin_base;

	for (i = 0; i < npctl->nbanks; i++) {
		ngpio = &npctl->ngpio[i];
		if (gpio >= ngpio->gc.base &&
				gpio < (ngpio->gc.base + ngpio->gc.ngpio)) {
			*gpio_idx = gpio - ngpio->gc.base;
			return ngpio;
		}
	}

	return NULL;
}

static int nuc980_pinctrl_post_conf(struct nuc980_pinctrl *npctl, int pin_idx)
{
	struct nuc980_pinctrl_gpio *ngpio;
	struct nuc980_pinctrl_pin *pin;
	unsigned long flags;
	unsigned gpio_idx;

	ngpio = nuc980_pinctrl_find_gpio_idx(npctl, pin_idx, &gpio_idx);

	if (!ngpio) {
		dev_warn(npctl->dev, "no GPIO range for pin %s\n",
					npctl->desc.pins[pin_idx].name);
		return -EINVAL;
	}

	pin = &npctl->pins[pin_idx];

	raw_spin_lock_irqsave(&ngpio->lock, flags);

	/* data must be set before the direction */
	if (pin->hwconf_data & CONF_SET)
		nuc980_pinctrl_gpio_reg(ngpio, REG_DOUT, gpio_idx, 1,
					pin->hwconf_data & CONF_MASK);

	if (pin->hwconf_bias & CONF_SET)
		nuc980_pinctrl_gpio_reg(ngpio, REG_PUSEL, gpio_idx, 2,
					pin->hwconf_bias & CONF_MASK);

	if (pin->hwconf_drive & CONF_SET)
		nuc980_pinctrl_gpio_out(ngpio, gpio_idx,
					pin->hwconf_drive & CONF_MASK, true);

	if (pin->hwconf_debounce & CONF_SET)
		nuc980_pinctrl_gpio_reg(ngpio, REG_DBEN, gpio_idx, 1,
					pin->hwconf_debounce & CONF_MASK);

	if (pin->hwconf_schmitt & CONF_SET)
		nuc980_pinctrl_gpio_reg(ngpio, REG_SMTEN, gpio_idx, 1,
					pin->hwconf_schmitt & CONF_MASK);

	if (pin->hwconf_input & CONF_SET)
		nuc980_pinctrl_gpio_reg(ngpio, REG_DINOFF, gpio_idx + 16, 1,
					pin->hwconf_input & CONF_MASK);

	/* dir must be set after the drive strength options */
	if (pin->hwconf_dir & CONF_SET) {
		if (pin->hwconf_dir & CONF_MASK) {
			nuc980_pinctrl_gpio_out(ngpio, gpio_idx, 0x00, false);
		} else {
			nuc980_pinctrl_gpio_reg(ngpio, REG_MODE, gpio_idx, 2, 0);
		}
	}

	raw_spin_unlock_irqrestore(&ngpio->lock, flags);

	return 0;
}

static int nuc980_pinctrl_get_groups_count(struct pinctrl_dev *pctldev)
{
	struct nuc980_pinctrl *npctl = pinctrl_dev_get_drvdata(pctldev);

	return npctl->desc.npins;
}

static const char *nuc980_pinctrl_get_group_name(struct pinctrl_dev *pctldev,
					unsigned group_idx)
{
	struct nuc980_pinctrl *npctl = pinctrl_dev_get_drvdata(pctldev);

	return npctl->group_names[group_idx];
}

static int nuc980_pinctrl_get_group_pins(struct pinctrl_dev *pctldev,
					unsigned group_idx,
					const unsigned **pins,
					unsigned *num_pins)
{
	struct nuc980_pinctrl *npctl = pinctrl_dev_get_drvdata(pctldev);

	*pins = &npctl->desc.pins[group_idx].number;
	*num_pins = 1;
	return 0;
}

static const struct pinctrl_ops nuc980_pinctrl_ops = {
	.get_groups_count = nuc980_pinctrl_get_groups_count,
	.get_group_name = nuc980_pinctrl_get_group_name,
	.get_group_pins = nuc980_pinctrl_get_group_pins,
	.dt_node_to_map = pinconf_generic_dt_node_to_map_pin,
	.dt_free_map = pinctrl_utils_free_map,
};

static int nuc980_pinctrl_get_funcs_count(struct pinctrl_dev *pctldev)
{
	return ARRAY_SIZE(nuc980_pinmux_functions);
}

static const char *nuc980_pinctrl_get_func_name(struct pinctrl_dev *pctldev,
					unsigned func_idx)
{
	return nuc980_pinmux_functions[func_idx];
}

static int nuc980_pinctrl_get_func_groups(struct pinctrl_dev *pctldev,
					unsigned func_idx,
					const char *const **group_names,
					unsigned *const num_groups)
{
	struct nuc980_pinctrl *npctl = pinctrl_dev_get_drvdata(pctldev);

	*group_names = npctl->group_names;
	*num_groups = npctl->desc.npins;

	return 0;
}

static int nuc980_pinctrl_set_mux(struct pinctrl_dev *pctldev,
					unsigned func_idx,
					unsigned group_idx)
{
	struct nuc980_pinctrl *npctl = pinctrl_dev_get_drvdata(pctldev);
	unsigned reg;
	int shift;
	int hwpin;

	hwpin = npctl->pins[group_idx].hwpin;
	reg = SYSCON_OFFSET + ((hwpin >> 3) << 2);
	shift = ((hwpin & 0x07) << 2);

	return regmap_update_bits(npctl->regmap, reg,
				0x0f << shift, func_idx << shift);
}

static int nuc980_pinctrl_reqen(struct pinctrl_dev *pctldev,
					struct pinctrl_gpio_range *range,
					unsigned range_idx)
{
	return 0;
}

static void nuc980_pinctrl_disfree(struct pinctrl_dev *pctldev,
					struct pinctrl_gpio_range *range,
					unsigned range_idx)
{
}

static int nuc980_pinctrl_set_direction(struct pinctrl_dev *pctldev,
					struct pinctrl_gpio_range *range,
					unsigned range_idx,
					bool input)
{
	return 0;
}

static const struct pinmux_ops nuc980_pinctrl_pinmux_ops = {
	.get_functions_count	= nuc980_pinctrl_get_funcs_count,
	.get_function_name	= nuc980_pinctrl_get_func_name,
	.get_function_groups	= nuc980_pinctrl_get_func_groups,
	.set_mux		= nuc980_pinctrl_set_mux,
	.gpio_request_enable    = nuc980_pinctrl_reqen,
	.gpio_disable_free      = nuc980_pinctrl_disfree,
	.gpio_set_direction	= nuc980_pinctrl_set_direction,
};

static int nuc980_pinctrl_find_pin_idx(struct pinctrl_dev *pctldev,
					unsigned pin)
{
	struct nuc980_pinctrl *npctl = pinctrl_dev_get_drvdata(pctldev);
	int pin_idx;

	for (pin_idx = 0; pin_idx < npctl->desc.npins; pin_idx++) {
		if (pin == npctl->desc.pins[pin_idx].number)
			return pin_idx;
	}

	return -ENODEV;
}

static int nuc980_pinctrl_pinconf_group_get(struct pinctrl_dev *pctldev,
					unsigned group_idx,
					unsigned long *config)
{
	struct nuc980_pinctrl *npctl = pinctrl_dev_get_drvdata(pctldev);

	*config = npctl->pins[group_idx].config;

	return 0;
}

static int nuc980_pinctrl_pinconf_group_set(struct pinctrl_dev *pctldev,
					unsigned group_idx,
					unsigned long *configs,
					unsigned num_configs)
{
	struct nuc980_pinctrl *npctl = pinctrl_dev_get_drvdata(pctldev);
	struct nuc980_pinctrl_gpio *ngpio;
	struct nuc980_pinctrl_pin *pin;
	enum pin_config_param param;
	enum pin_config_param arg;
	unsigned gpio_idx;
	int ret;
	int i;

	pin = &npctl->pins[group_idx];

	if (npctl->initialized) {
		ngpio = nuc980_pinctrl_find_gpio_idx(npctl,
						group_idx, &gpio_idx);
		if (!ngpio) {
			dev_warn(npctl->dev, "no GPIO range for pin %s\n",
					npctl->desc.pins[group_idx].name);
			return -EINVAL;
		}

		for (i = 0; i < num_configs; i++) {
			ret = nuc980_pinctrl_gpio_set_config(&ngpio->gc,
					gpio_idx, configs[i]);
			if (ret)
				return ret;

			pin->config = configs[i];
		}

		return 0;
	}

	for (i = 0; i < num_configs; i++) {
		param = pinconf_to_config_param(configs[i]);
		arg = pinconf_to_config_argument(configs[i]);

		switch (param) {
		case PIN_CONFIG_BIAS_DISABLE:
		case PIN_CONFIG_BIAS_PULL_PIN_DEFAULT:
		case PIN_CONFIG_BIAS_HIGH_IMPEDANCE:
			pin->hwconf_bias = CONF_SET | 0x00;
			break;

		case PIN_CONFIG_BIAS_PULL_DOWN:
			pin->hwconf_bias = CONF_SET | 0x02;
			break;

		case PIN_CONFIG_BIAS_PULL_UP:
			pin->hwconf_bias = CONF_SET | 0x01;
			break;

		case PIN_CONFIG_DRIVE_OPEN_DRAIN:
			pin->hwconf_drive = CONF_SET | 0x02;
			break;

		case PIN_CONFIG_DRIVE_PUSH_PULL:
			pin->hwconf_drive = CONF_SET | 0x01;
			break;

		case PIN_CONFIG_DRIVE_STRENGTH:
			// TODO: only for port B
			break;

		case PIN_CONFIG_DRIVE_STRENGTH_UA:
			// TODO: only for port B
			break;

		case PIN_CONFIG_INPUT_DEBOUNCE:
			pin->hwconf_debounce = CONF_SET | !!arg;
			break;

		case PIN_CONFIG_INPUT_ENABLE:
			pin->hwconf_input = CONF_SET | !arg;
			break;

		case PIN_CONFIG_INPUT_SCHMITT_ENABLE:
			pin->hwconf_schmitt = CONF_SET | !!arg;
			break;

		case PIN_CONFIG_OUTPUT_ENABLE:
			pin->hwconf_dir = CONF_SET | !!arg;
			break;

		case PIN_CONFIG_OUTPUT:
			pin->hwconf_data = CONF_SET | !!arg;
			pin->hwconf_dir = CONF_SET | 0x01;
			break;

		case PIN_CONFIG_SLEW_RATE:
			// TODO:
			break;

		default:
			return -ENOTSUPP;
		}

		npctl->pins[group_idx].config = configs[i];
	}

	return 0;
}

static int nuc980_pinctrl_pinconf_get(struct pinctrl_dev *pctldev,
					unsigned pin,
					unsigned long *config)
{
	int pin_idx;

	pin_idx = nuc980_pinctrl_find_pin_idx(pctldev, pin);
	if (pin_idx < 0)
		return -ENOTSUPP;

	return nuc980_pinctrl_pinconf_group_get(pctldev, pin_idx, config);
}

static int nuc980_pinctrl_pinconf_set(struct pinctrl_dev *pctldev,
					unsigned pin,
					unsigned long *configs,
					unsigned num_configs)
{
	int pin_idx;

	pin_idx = nuc980_pinctrl_find_pin_idx(pctldev, pin);
	if (pin_idx < 0)
		return -ENOTSUPP;

	return nuc980_pinctrl_pinconf_group_set(pctldev, pin_idx,
						configs, num_configs);
}

static const struct pinconf_ops nuc980_pinctrl_pinconf_ops = {
	.pin_config_group_get = nuc980_pinctrl_pinconf_group_get,
	.pin_config_group_set = nuc980_pinctrl_pinconf_group_set,
	.pin_config_get       = nuc980_pinctrl_pinconf_get,
	.pin_config_set       = nuc980_pinctrl_pinconf_set,
};

static int nuc980_pinctrl_probe(struct platform_device *pdev)
{
	struct nuc980_pinctrl *npctl;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *gpio_np;
	struct pinctrl_pin_desc *pins_desc;
	struct property *nuvoton_npins;
	struct nuc980_pinctrl_gpio *ngpio;
	unsigned gpio_idx;
	int max_npins;
	int npins;
	u32 pin;
	int i;
	int j;

	dev_set_name(dev, "regmap.nuc980-pinctrl");

	npctl = devm_kzalloc(dev, sizeof(*npctl), GFP_KERNEL);
	if (!npctl) {
		dev_err(dev, "unable to allocate memory\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, npctl);

	npctl->regmap = syscon_regmap_lookup_by_phandle(np, "syscon");
	if (IS_ERR(npctl->regmap)) {
		dev_err(dev, "unable to get syscon region\n");
		return PTR_ERR(npctl->regmap);
	}

	if (regmap_get_max_register(npctl->regmap) < SYSCON_OFFSET) {
		dev_err(dev, "syscon region size out of range\n");
		return -EINVAL;
	}

	max_npins = regmap_get_max_register(npctl->regmap) + 4 - SYSCON_OFFSET;
	max_npins = min((max_npins * 2), 256);

	nuvoton_npins = of_find_property(np, "nuvoton,pins", NULL);
	if (!nuvoton_npins) {
		dev_err(dev, "missing nuvoton,pins property\n");
		return -EINVAL;
	}

	npins = nuvoton_npins->length / sizeof(u32);
	if (npins > max_npins) {
		dev_warn(dev, "nuvoton,pins out of range\n");
		npins = max_npins;
	}

	npctl->desc.npins = 0;

	for (i = 0; i < npins; i++) {
		if (of_property_read_u32_index(np, "nuvoton,pins", i, &pin))
			break;

		if (pin)
			npctl->desc.npins++;
	}

	if (npctl->desc.npins) {
		pins_desc = devm_kcalloc(dev, npctl->desc.npins,
				sizeof(*pins_desc), GFP_KERNEL);

		npctl->group_names = devm_kcalloc(dev, npctl->desc.npins,
				sizeof(*npctl->group_names), GFP_KERNEL);

		npctl->pins = devm_kcalloc(dev, npctl->desc.npins,
				sizeof(*npctl->pins), GFP_KERNEL);

		if (!pins_desc || !npctl->group_names || !npctl->pins) {
			dev_err(dev, "unable to allocate memory\n");
			return -ENOMEM;
		}

		for (i = 0, j = 0; i < npins; i++) {
			if (of_property_read_u32_index(np, "nuvoton,pins",
								i, &pin))
				break;

			if (pin) {
				npctl->group_names[j] = devm_kasprintf(dev,
					GFP_KERNEL, "P%c%d",
					(int)('A' + (i / 16)), (i % 16));
				npctl->pins[j].hwpin = i;
				pins_desc[j].number = pin;
				pins_desc[j].drv_data = npctl;
				pins_desc[j].name = npctl->group_names[j];
				j++;
			}
		}
	} else {
		dev_warn(dev, "no pins configured\n");

		pins_desc = NULL;
		npctl->group_names = NULL;
	}

	npctl->dev = dev;
	npctl->np = np;
	npctl->desc.name = dev_name(dev);
	npctl->desc.pctlops = &nuc980_pinctrl_ops;
	npctl->desc.pmxops = &nuc980_pinctrl_pinmux_ops;
	npctl->desc.confops = &nuc980_pinctrl_pinconf_ops;
	npctl->desc.pins = pins_desc;
	npctl->desc.owner = THIS_MODULE;

	npctl->pctl = devm_pinctrl_register(dev, &npctl->desc, npctl);

	if (IS_ERR(npctl->pctl)) {
		dev_err(dev, "unable to add pinctrl\n");
		return PTR_ERR(npctl->pctl);
	}

	/* GPIO */
	for_each_available_child_of_node(dev->of_node, gpio_np) {
		if (!of_device_is_compatible(gpio_np, "nuvoton,nuc980-gpio"))
			continue;

		if (!of_find_property(gpio_np, "gpio-controller", NULL)) {
			dev_warn(dev, "%s: not a gpio-controller\n",
							np->full_name);
			continue;
		}
		npctl->nbanks++;
	}

	dev_info(dev, "found %d gpio banks\n", npctl->nbanks);
	npctl->ngpio = devm_kcalloc(dev, npctl->nbanks, sizeof(*npctl->ngpio),
							GFP_KERNEL);

	if (!npctl->ngpio) {
		dev_warn(dev, "unable to allocate memory\n");
		npctl->nbanks = 0;
	}

	i = 0;
	for_each_available_child_of_node(dev->of_node, gpio_np) {
		if (i >= npctl->nbanks)
			break;

		if (!of_device_is_compatible(gpio_np, "nuvoton,nuc980-gpio"))
			continue;

		if (!of_find_property(gpio_np, "gpio-controller", NULL)) {
			dev_warn(dev, "%s: not a gpio-controller\n",
							gpio_np->full_name);
			continue;
		}

		nuc980_pinctrl_gpio_probe(&npctl->ngpio[i], dev,
							gpio_np, npctl);
		i++;
	}

	npctl->initialized = true;

	for (i = 0; i < npctl->desc.npins; i++) {
		ngpio = nuc980_pinctrl_find_gpio_idx(npctl, i, &gpio_idx);
		if (ngpio)
			ngpio->pin_idx[gpio_idx] = i;

		nuc980_pinctrl_post_conf(npctl, i);
	}

	dev_info(dev, "initialized\n");

	return 0;
}

static const struct of_device_id nuc980_pinctrl_dt_ids[] = {
	{ .compatible = "nuvoton,nuc980-pinctrl" },
	{},
};
MODULE_DEVICE_TABLE(of, nuc980_pinctrl_dt_ids);

static struct platform_driver nuc980_pinctrl_driver = {
	.probe = nuc980_pinctrl_probe,
	.driver = {
		.name           = "nuc980-pinctrl",
		.of_match_table = nuc980_pinctrl_dt_ids,
	},
};

static int __init nuc980_pinctrl_init(void)
{
	return platform_driver_register(&nuc980_pinctrl_driver);
}
subsys_initcall(nuc980_pinctrl_init);
