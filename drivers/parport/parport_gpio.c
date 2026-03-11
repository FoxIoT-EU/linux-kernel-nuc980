// SPDX-License-Identifier: GPL-2.0-or-later

/* Low-level parallel port routines for GPIO.
 *
 * Author: Jim Garlick <garlick.jim@gmail.com>
 *
 * Based on parport_sunbpp.c.
 *
 * A PC parallel port style interface is cobbled together from GPIO pins:
 * 8-bit data out, 5-bit status in, 4 bit control out
 * No interrupts or dma; SPP mode only.
 *
 * See https://lwn.net/Articles/533632/ for info on the gpiod_ API used here.
 *
 * 2020.10.23: Added IRQ support. Veiko Rütter <rebane@alkohol.ee>
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/parport.h>
#include <linux/slab.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/of_irq.h>

struct parport_gpio_ctx {
	struct gpio_descs *data;
	struct gpio_descs *status;
	struct gpio_descs *control;
	struct gpio_desc *hd; // v2 only: 74LVC161284 HD pin
	struct gpio_desc *dir; // v2 only: 74LVC161284 DIR pin
	spinlock_t lock;
	int irq_enabled;
};

static unsigned char parport_gpio_read_data(struct parport *p)
{
	struct parport_gpio_ctx *ctx = p->private_data;
	unsigned char data;
	unsigned long flags;

	spin_lock_irqsave(&ctx->lock, flags);

	data = gpiod_get_value(ctx->data->desc[0]);
	data |= (gpiod_get_value(ctx->data->desc[1]) << 1);
	data |= (gpiod_get_value(ctx->data->desc[2]) << 2);
	data |= (gpiod_get_value(ctx->data->desc[3]) << 3);
	data |= (gpiod_get_value(ctx->data->desc[4]) << 4);
	data |= (gpiod_get_value(ctx->data->desc[5]) << 5);
	data |= (gpiod_get_value(ctx->data->desc[6]) << 6);
	data |= (gpiod_get_value(ctx->data->desc[7]) << 7);

	spin_unlock_irqrestore(&ctx->lock, flags);

	return data;
}

static void parport_gpio_write_data(struct parport *p, unsigned char data)
{
	struct parport_gpio_ctx *ctx = p->private_data;
	unsigned long value_bitmap = data;
	unsigned long flags;

	spin_lock_irqsave(&ctx->lock, flags);

	gpiod_set_array_value(ctx->data->ndescs, ctx->data->desc, NULL,
							&value_bitmap);

	spin_unlock_irqrestore(&ctx->lock, flags);
}

static unsigned char parport_gpio_read_control(struct parport *p)
{
	struct parport_gpio_ctx *ctx = p->private_data;
	unsigned char control;
	unsigned long flags;

	spin_lock_irqsave(&ctx->lock, flags);

	control = gpiod_get_value(ctx->control->desc[0]);
	control |= gpiod_get_value(ctx->control->desc[1]) << 1;
	control |= gpiod_get_value(ctx->control->desc[2]) << 2;
	control |= gpiod_get_value(ctx->control->desc[3]) << 3;

	spin_unlock_irqrestore(&ctx->lock, flags);

	return control;
}

static void parport_gpio_write_control(struct parport *p, unsigned char control)
{
	struct parport_gpio_ctx *ctx = p->private_data;
	unsigned long value_bitmap = control;
	unsigned long flags;

	spin_lock_irqsave(&ctx->lock, flags);

	gpiod_set_array_value(ctx->control->ndescs, ctx->control->desc, NULL,
							&value_bitmap);

	spin_unlock_irqrestore(&ctx->lock, flags);
}

static unsigned char parport_gpio_frob_control(struct parport *p,
					       unsigned char mask,
					       unsigned char val)
{
	struct parport_gpio_ctx *ctx = p->private_data;
	unsigned long flags;

	spin_lock_irqsave(&ctx->lock, flags);

	if ((mask & 1)) { // ~Strobe
		gpiod_set_value(ctx->control->desc[0], val & 1);
	}
	if ((mask & 2)) { // ~nAutoLF
		gpiod_set_value(ctx->control->desc[1], (val >> 1) & 1);
	}
	if ((mask & 4)) { // nInitialize
		gpiod_set_value(ctx->control->desc[2], (val >> 2) & 1);
	}
	if ((mask & 8)) { // ~nSelect
		gpiod_set_value(ctx->control->desc[3], (val >> 3) & 1);
	}

	spin_unlock_irqrestore(&ctx->lock, flags);

	return parport_gpio_read_control(p);
}

static unsigned char parport_gpio_read_status(struct parport *p)
{
	struct parport_gpio_ctx *ctx = p->private_data;
	unsigned char status;
	unsigned long flags;

	spin_lock_irqsave(&ctx->lock, flags);

	status = gpiod_get_value(ctx->status->desc[0]) << 3; // nError
	status |= gpiod_get_value(ctx->status->desc[1]) << 4; // Select
	status |= gpiod_get_value(ctx->status->desc[2]) << 5; // Paperout
	status |= gpiod_get_value(ctx->status->desc[3]) << 6; // nAck
	status |= gpiod_get_value(ctx->status->desc[4]) << 7; //~Busy

	spin_unlock_irqrestore(&ctx->lock, flags);

	return status;
}

static void parport_gpio_init_state(struct pardevice *d,
				    struct parport_state *s)
{
}

static void parport_gpio_save_state(struct parport *p, struct parport_state *s)
{
}

static void parport_gpio_restore_state(struct parport *p,
				       struct parport_state *s)
{
}

static void parport_gpio_enable_irq(struct parport *p)
{
	struct parport_gpio_ctx *ctx = p->private_data;
	unsigned long flags;

	if (p->irq == PARPORT_IRQ_NONE)
		return;

	local_irq_save(flags);
	if (!ctx->irq_enabled) {
		enable_irq(p->irq);
		ctx->irq_enabled = 1;
	}
	local_irq_restore(flags);
}

static void parport_gpio_disable_irq(struct parport *p)
{
	struct parport_gpio_ctx *ctx = p->private_data;
	unsigned long flags;

	if (p->irq == PARPORT_IRQ_NONE)
		return;

	local_irq_save(flags);
	if (ctx->irq_enabled) {
		disable_irq(p->irq);
		ctx->irq_enabled = 0;
	}
	local_irq_restore(flags);
}

static void parport_gpio_data_forward(struct parport *p)
{
	struct parport_gpio_ctx *ctx = p->private_data;

	if (ctx->dir) {
		int i;

		for (i = 0; i < 8; i++) {
			if (gpiod_direction_output(ctx->data->desc[i],
						   GPIOD_OUT_LOW) < 0)
				dev_err(p->dev, "%s data%d\n", __func__, i);
		}
		gpiod_set_value(ctx->dir, 1);
	}
}

static void parport_gpio_data_reverse(struct parport *p)
{
	struct parport_gpio_ctx *ctx = p->private_data;
	int i;

	if (ctx->dir) {
		for (i = 0; i < 8; i++) {
			if (gpiod_direction_input(ctx->data->desc[i]) < 0)
				dev_err(p->dev, "%s data%d\n", __func__, i);
		}
		gpiod_set_value(ctx->dir, 0);
	}
}

static struct parport_operations parport_gpio_ops = {
	.write_data = parport_gpio_write_data,
	.read_data = parport_gpio_read_data,

	.write_control = parport_gpio_write_control,
	.read_control = parport_gpio_read_control,
	.frob_control = parport_gpio_frob_control,

	.read_status = parport_gpio_read_status,

	.enable_irq = parport_gpio_enable_irq,
	.disable_irq = parport_gpio_disable_irq,

	.data_forward = parport_gpio_data_forward,
	.data_reverse = parport_gpio_data_reverse,

	.init_state = parport_gpio_init_state,
	.save_state = parport_gpio_save_state,
	.restore_state = parport_gpio_restore_state,

	.epp_write_data = parport_ieee1284_epp_write_data,
	.epp_read_data = parport_ieee1284_epp_read_data,
	.epp_write_addr = parport_ieee1284_epp_write_addr,
	.epp_read_addr = parport_ieee1284_epp_read_addr,

	.ecp_write_data = parport_ieee1284_ecp_write_data,
	.ecp_read_data = parport_ieee1284_ecp_read_data,
	.ecp_write_addr = parport_ieee1284_ecp_write_addr,

	.compat_write_data = parport_ieee1284_write_compat,
	.nibble_read_data = parport_ieee1284_read_nibble,
	.byte_read_data = parport_ieee1284_read_byte,

	.owner = THIS_MODULE,
};

static void parport_gpio_print_info(struct parport *p)
{
	struct parport_gpio_ctx *ctx = p->private_data;

	dev_info(p->dev, "data on pins [%d,%d,%d,%d,%d,%d,%d,%d]\n",
		desc_to_gpio(ctx->data->desc[7]),
		desc_to_gpio(ctx->data->desc[6]),
		desc_to_gpio(ctx->data->desc[5]),
		desc_to_gpio(ctx->data->desc[4]),
		desc_to_gpio(ctx->data->desc[3]),
		desc_to_gpio(ctx->data->desc[2]),
		desc_to_gpio(ctx->data->desc[1]),
		desc_to_gpio(ctx->data->desc[0]));
	dev_info(p->dev, "status on pins [%d,%d,%d,%d,%d]\n",
		desc_to_gpio(ctx->status->desc[4]),
		desc_to_gpio(ctx->status->desc[3]),
		desc_to_gpio(ctx->status->desc[2]),
		desc_to_gpio(ctx->status->desc[1]),
		desc_to_gpio(ctx->status->desc[0]));
	dev_info(p->dev, "control on pins [%d,%d,%d,%d]\n",
		desc_to_gpio(ctx->control->desc[3]),
		desc_to_gpio(ctx->control->desc[2]),
		desc_to_gpio(ctx->control->desc[1]),
		desc_to_gpio(ctx->control->desc[0]));
	if (ctx->hd)
		dev_info(p->dev, "hd on pin %d\n",
			desc_to_gpio(ctx->hd));
	if (ctx->dir)
		dev_info(p->dev, "dir on pin %d\n",
			desc_to_gpio(ctx->dir));
}

static void parport_gpio_detach(struct parport_gpio_ctx *ctx)
{
	if (ctx) {
		if (ctx->data)
			gpiod_put_array(ctx->data);
		if (ctx->status)
			gpiod_put_array(ctx->status);
		if (ctx->control)
			gpiod_put_array(ctx->control);
		if (ctx->hd)
			gpiod_put(ctx->hd);
		if (ctx->dir)
			gpiod_put(ctx->dir);
		kfree(ctx);
	}
}

static int parport_gpio_attach(struct device *dev,
			       struct parport_gpio_ctx **ctxp)
{
	struct parport_gpio_ctx *ctx;
	int i;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		goto out;
	ctx->data = gpiod_get_array_optional(dev, "data", GPIOD_OUT_LOW);
	if (IS_ERR_OR_NULL(ctx->data) || ctx->data->ndescs != 8) {
		dev_err(dev, "could not get data pins\n");
		if (IS_ERR(ctx->data))
			ctx->data = NULL;
		goto out;
	}
	ctx->status = gpiod_get_array_optional(dev, "status", GPIOD_IN);
	if (IS_ERR_OR_NULL(ctx->status) || ctx->status->ndescs != 5) {
		dev_err(dev, "could not get status pins\n");
		if (IS_ERR(ctx->status))
			ctx->status = NULL;
		goto out;
	}
	ctx->control = gpiod_get_array_optional(dev, "control", GPIOD_OUT_LOW);
	if (IS_ERR_OR_NULL(ctx->control) || ctx->control->ndescs != 4) {
		dev_err(dev, "could not get control pins\n");
		if (IS_ERR(ctx->control))
			ctx->control = NULL;
		goto out;
	}
	for (i = 0; i < ctx->data->ndescs; i++) {
		if (gpiod_cansleep(ctx->data->desc[i]))
			goto out_cansleep;
	}
	for (i = 0; i < ctx->status->ndescs; i++) {
		if (gpiod_cansleep(ctx->status->desc[i]))
			goto out_cansleep;
	}
	for (i = 0; i < ctx->control->ndescs; i++) {
		if (gpiod_cansleep(ctx->control->desc[i]))
			goto out_cansleep;
	}
	/* v2 hardware design has SN74LVBC161284 HD and DIR pins.
	 * If device tree overlay defines these, initialize:
	 * DIR: 1=data flows in the A-B direction (not B-A)
	 * HD: 1=outputs in totem pole config (not open drain)
	 */
	ctx->hd = gpiod_get_optional(dev, "hd", GPIOD_OUT_HIGH);
	if (ctx->hd && gpiod_cansleep(ctx->hd))
		goto out_cansleep;
	ctx->dir = gpiod_get_optional(dev, "dir", GPIOD_OUT_HIGH);
	if (ctx->dir && gpiod_cansleep(ctx->dir))
		goto out_cansleep;

	spin_lock_init(&ctx->lock);
	*ctxp = ctx;
	return 0;
out_cansleep:
	dev_err(dev, "inappropriate gpio pin (can sleep)\n");
out:
	parport_gpio_detach(ctx);
	return -EINVAL;
}

static int parport_gpio_probe(struct platform_device *op)
{
	struct parport_gpio_ctx *ctx = NULL;
	struct parport *p;
	unsigned long irqflags;
	int irq;
	int ret;

	if (parport_gpio_attach(&op->dev, &ctx) < 0)
		goto out;

	irq = irq_of_parse_and_map(op->dev.of_node, 0);
	if (irq <= 0) {
		irq = PARPORT_IRQ_NONE;
		dev_warn(&op->dev, "no IRQ, only SPP mode\n");
	} else {
		irqflags = irq_get_trigger_type(irq);
	}

	p = parport_register_port(0, irq, PARPORT_DMA_NONE, &parport_gpio_ops);
	if (!p) {
		dev_err(&op->dev, "parport_register_port\n");
		goto out_detach;
	}
	p->private_data = ctx;
	p->modes = PARPORT_MODE_PCSPP;
	p->dev = &op->dev;

	dev_set_drvdata(&op->dev, p);

	if (p->irq != PARPORT_IRQ_NONE) {
		ret = devm_request_irq(&op->dev, p->irq, parport_irq_handler,
					irqflags, dev_name(&op->dev), p);

		if (ret) {
			dev_err(&op->dev, "unable to request irq\n");
			goto out_put_port;
		}

		ctx->irq_enabled = 1;
	}

	parport_gpio_print_info(p);

	parport_announce_port(p);
	return 0;
out_put_port:
	parport_put_port(p);
out_detach:
	parport_gpio_detach(ctx);
out:
	return -ENODEV;
}

static void parport_gpio_remove(struct platform_device *op)
{
	struct parport *p = dev_get_drvdata(&op->dev);
	struct parport_gpio_ctx *ctx = p->private_data;

	parport_remove_port(p);
	p->private_data = NULL;
	parport_put_port(p);

	parport_gpio_detach(ctx);
}

static const struct of_device_id parport_gpio_match[] = {
	{ .compatible = "parport-gpio",
	},
	{},
};

MODULE_DEVICE_TABLE(of, parport_gpio_match);

static struct platform_driver parport_gpio_driver = {
	.driver = {
		.name = "parport-gpio",
		.of_match_table = parport_gpio_match,
	},
	.probe = parport_gpio_probe,
	.remove = parport_gpio_remove,
};

module_platform_driver(parport_gpio_driver);

MODULE_AUTHOR("Jim Garlick");
MODULE_DESCRIPTION("Parport Driver for Raspberry Pi GPIO Parallel Port HAT");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL");
