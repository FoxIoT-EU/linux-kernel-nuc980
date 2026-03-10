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
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/trigger.h>
#include <linux/iio/buffer.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/slab.h>

#define REG_CTL              0x000
#define   CTL_ADEN           BIT(0)
#define   CTL_VBGEN          BIT(1)
#define   CTL_MST            BIT(8)
#define REG_CONF             0x004
#define   CONF_NACEN         BIT(2)
#define   CONF_REFSEL_AVDD33 (0x03 << 6)
#define   CONF_CHANNEL_MASK  (0x0f << 12)
#define   CONF_CHANNEL_SHIFT 12
#define REG_IER              0x008
#define   IER_MIEN           BIT(0)
#define REG_ISR              0x00C
#define   ISR_MF             BIT(0)
#define   ISR_NACF           BIT(10)
#define REG_DATA             0x028
#define NUC980_ADC_REG_SIZE  0x100

#define NUC980_ADC_TIMEOUT   (msecs_to_jiffies(1000))

#define NUC980_ADC_CHANNEL(_index, _id) {                     \
	.type                     = IIO_VOLTAGE,              \
	.indexed                  = 1,                        \
	.channel                  = _index,                   \
	.address                  = _index,                   \
	.info_mask_separate       = BIT(IIO_CHAN_INFO_RAW) |  \
	                            BIT(IIO_CHAN_INFO_SCALE), \
	.datasheet_name           = _id,                      \
	.scan_index               = _index,                   \
	.scan_type                = {                         \
		.sign             = 'u',                      \
		.realbits         = 12,                       \
		.storagebits      = 16,                       \
		.shift            = 0,                        \
		.endianness       = IIO_BE,                   \
	},                                                    \
}

struct nuc980_adc {
	struct device        *dev;
	void __iomem         *base;
	struct clk           *clk;
	struct reset_control *rst;
	int                  irq;
	struct completion    completion;
	struct iio_trigger   *trig;
	struct regulator     *vref;
	u32                  vref_uv;
	struct mutex         reg_lock;
};

static const struct iio_chan_spec nuc980_adc_iio_channels[] = {
	NUC980_ADC_CHANNEL(0, "adc0"),
	NUC980_ADC_CHANNEL(1, "adc1"),
	NUC980_ADC_CHANNEL(2, "adc2"),
	NUC980_ADC_CHANNEL(3, "adc3"),
	NUC980_ADC_CHANNEL(4, "adc4"),
	NUC980_ADC_CHANNEL(5, "adc5"),
	NUC980_ADC_CHANNEL(6, "adc6"),
	NUC980_ADC_CHANNEL(7, "adc7"),
};

static irqreturn_t nuc980_adc_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *iio = pf->indio_dev;
	struct nuc980_adc *nadc = iio_priv(iio);
	unsigned long timeout;
	int channel;
	int val;
	u32 reg;

	channel = find_first_bit(iio->active_scan_mask, iio->masklength);

	reg = readl(nadc->base + REG_CONF);
	reg = (reg & ~CONF_CHANNEL_MASK) | (channel << CONF_CHANNEL_SHIFT);
	writel(reg, nadc->base + REG_CONF);

	writel(readl(nadc->base + REG_CTL) | CTL_MST, nadc->base + REG_CTL);

	timeout = wait_for_completion_interruptible_timeout
	          (&nadc->completion, NUC980_ADC_TIMEOUT);

	val = readl(nadc->base + REG_DATA);

	iio_push_to_buffers(iio, (void *)&val);
	iio_trigger_notify_done(iio->trig);

	return IRQ_HANDLED;
}

static irqreturn_t nuc980_adc_irq(int irq, void *dev_id)
{
	struct nuc980_adc *nadc = dev_id;

	if (readl(nadc->base + REG_ISR) & ISR_MF) {
		writel(ISR_NACF | ISR_MF, nadc->base + REG_ISR);
		complete(&nadc->completion);
	}

	return IRQ_HANDLED;
}

static int nuc980_adc_read_raw(struct iio_dev *iio,
                               struct iio_chan_spec const *chan,
                               int *val, int *val2, long mask)
{
	struct nuc980_adc *nadc = iio_priv(iio);
	unsigned long timeout;
	u32 reg;
	int i;

	if (mask == IIO_CHAN_INFO_SCALE) {
		*val = nadc->vref_uv / 1000;
		*val2 = 12;
		return IIO_VAL_FRACTIONAL_LOG2;
	}

	if (mask != IIO_CHAN_INFO_RAW)
		return -EINVAL;

	mutex_lock(&nadc->reg_lock);

	for (i = 0; i < 2; i++) {
		reg = readl(nadc->base + REG_CONF);
		reg = (reg & ~CONF_CHANNEL_MASK) |
					(chan->channel << CONF_CHANNEL_SHIFT);
		writel(reg, nadc->base + REG_CONF);

		writel(readl(nadc->base + REG_CTL) | CTL_MST, nadc->base + REG_CTL);

		timeout = wait_for_completion_interruptible_timeout
			  (&nadc->completion, NUC980_ADC_TIMEOUT);

		*val = readl(nadc->base + REG_DATA);
//		if (timeout && (*val != 0))
//			break;
	}

	mutex_unlock(&nadc->reg_lock);

	if (timeout == 0)
		return -ETIMEDOUT;

	return IIO_VAL_INT;
}

static const struct iio_buffer_setup_ops nuc980_adc_ring_setup_ops = {
//	.postenable = &iio_triggered_buffer_postenable,
//	.predisable = &iio_triggered_buffer_predisable,
};

static const struct iio_info nuc980_adc_info = {
	.read_raw = &nuc980_adc_read_raw,
};

static int nuc980_adc_probe(struct platform_device *pdev)
{
	struct nuc980_adc *nadc;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct iio_dev	*iio;
	struct resource res;
	int ret;

	iio = devm_iio_device_alloc(dev, sizeof(*nadc));
	if (!iio) {
		dev_err(dev, "unable to allocate memory\n");
		return -ENOMEM;
	}

	nadc = iio_priv(iio);
	platform_set_drvdata(pdev, nadc);

	ret = of_address_to_resource(np, 0, &res);
	if (ret) {
		dev_err(dev, "unable to get mem region\n");
		return ret;
	}

	if (!res.start || resource_size(&res) < NUC980_ADC_REG_SIZE) {
		dev_err(dev, "mem region out of range\n");
		return -EINVAL;
	}

	dev_set_name(dev, "%08x.nuc980-adc", (unsigned int)res.start);

	nadc->base = devm_ioremap_resource(dev, &res);
	if (IS_ERR(nadc->base)) {
		dev_err(dev, "unable to map mem region\n");
                return PTR_ERR(nadc->base);
	}

	nadc->vref = devm_regulator_get(&pdev->dev, "vref");
	if (IS_ERR(nadc->vref)) {
		dev_err(dev, "unable to get vref supply\n");
		return PTR_ERR(nadc->vref);
	}

	ret = regulator_enable(nadc->vref);
	if (ret) {
		dev_err(dev, "unable to enable vref supply\n");
		return ret;
	}

	nadc->vref_uv = regulator_get_voltage(nadc->vref);

	nadc->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(nadc->clk)) {
		dev_warn(dev, "unable to get clock\n");
		nadc->clk = NULL;
	}

	ret = clk_prepare_enable(nadc->clk);
	if (ret) {
		dev_err(dev, "unable to enable clock\n");
		return ret;
	}

	nadc->rst = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (!IS_ERR(nadc->rst))
		reset_control_reset(nadc->rst);

	nadc->irq = irq_of_parse_and_map(np, 0);
	if (nadc->irq <= 0) {
		dev_err(dev, "unable to get irq\n");
		ret = -EINVAL;
		goto disable_clock;
	}

	iio->dev.parent = dev;
	iio->name = dev_name(dev);
	iio->modes = INDIO_DIRECT_MODE;
	iio->info = &nuc980_adc_info;
	iio->num_channels = 8;
	iio->channels = nuc980_adc_iio_channels;
	iio->masklength = iio->num_channels - 1;

	init_completion(&nadc->completion);
	mutex_init(&nadc->reg_lock);

	ret = devm_request_irq(dev, nadc->irq, nuc980_adc_irq,
	                  0, dev_name(dev), nadc);

	if (ret < 0) {
		dev_err(dev, "unable to request irq\n");
		goto disable_clock;
	}

#ifdef CONFIG_NUC980ADC_VREF
	writel(CTL_ADEN, nadc->base + REG_CTL);
#endif
//#ifdef CONFIG_NUC980ADC_I33V
	writel(0x3 << 6, nadc->base + REG_CONF); //select AGND33 vs AVDD33
	writel(CTL_ADEN, nadc->base + REG_CTL);
//#endif
#ifdef CONFIG_NUC980ADC_BANDGAP
	writel(readl(nadc->base + REG_CTL) | CTL_VBGEN, nadc->base + REG_CTL);
#endif

	writel(IER_MIEN, nadc->base + REG_IER);

	ret = iio_triggered_buffer_setup(iio, &iio_pollfunc_store_time,
		&nuc980_adc_trigger_handler, &nuc980_adc_ring_setup_ops);

	if (ret) {
		dev_err(dev, "unable to setup triggered buffer\n");
		goto disable_clock;
	}

	ret = iio_device_register(iio);
	if (ret < 0) {
		dev_err(dev, "unable to register iio device\n");
		goto disable_clock;
	}

	writel(readl(nadc->base + REG_CONF) | CONF_NACEN | (64 << 24) | (6 << 16),
						nadc->base + REG_CONF);

	dev_info(dev, "initialized\n");

	return 0;

disable_clock:
	clk_disable_unprepare(nadc->clk);
	return ret;
}

static const struct of_device_id nuc980_adc_dt_ids[] = {
	{ .compatible = "nuvoton,nuc980-adc" },
	{},
};
MODULE_DEVICE_TABLE(of, nuc980_adc_dt_ids);

static struct platform_driver nuc980_adc_driver = {
	.probe	= nuc980_adc_probe,
	.driver = {
		.name   = "nuc980-adc",
		.of_match_table = nuc980_adc_dt_ids,
	},
};

static int __init nuc980_adc_init(void)
{
	return platform_driver_register(&nuc980_adc_driver);
}
device_initcall(nuc980_adc_init);
