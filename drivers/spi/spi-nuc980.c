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
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/spi/spi.h>

#define REG_CTL               0x00
#define   CTL_SPIEN           (0x01 << 0)
#define   CTL_RXNEG           (0x01 << 1)
#define   CTL_TXNEG           (0x01 << 2)
#define   CTL_CLKPOL          (0x01 << 3)
#define   CTL_LSB             (0x01 << 13)
#define   CTL_DATDIR          (0x01 << 20)
#define   CTL_DUALIOEN        (0x01 << 21)
#define   CTL_QUADIOEN        (0x01 << 22)
#define REG_CLKDIV            0x04
#define REG_SSCTL             0x08
#define REG_PDMACTL           0x0c
#define REG_FIFOCTL           0x10
#define   FIFOCTL_RXRST       (0x01 << 0)
#define   FIFOCTL_TXRST       (0x01 << 1)
#define   FIFOCTL_RXTHIEN     (0x01 << 2)
#define REG_STATUS            0x14
#define   STATUS_RXEMPTY      (0x01 << 8)
#define   STATUS_ENSTS        (0x01 << 15)
#define   STATUS_TXRXRST      (0x01 << 23)
#define REG_TX                0x20
#define REG_RX                0x30
#define NUC980_SPI_REG_SIZE   0x40

#define NUC980_SPI_MAX_NUM_CS 2

/* Timeout for SPI enable/disable and FIFO reset */
#define NUC980_SPI_CTL_TIMEOUT_US    1000
/* Timeout waiting for RX data per byte */
#define NUC980_SPI_RX_TIMEOUT_US     10000
/* Timeout for IRQ-driven completion */
#define NUC980_SPI_XFER_TIMEOUT_MS   2000

struct nuc980_spi {
	struct spi_controller *master;
	void __iomem          *base;
	struct clk            *pclk;
	struct clk            *eclk;
	struct completion     done;
	unsigned long         rate;
	u8                    *rx_buf;
	const u8              *tx_buf;
	int                   rx_len;
	int                   tx_len;
	int                   threshold;
};

static void nuc980_spi_cs_assert(struct spi_device *spi)
{
	struct nuc980_spi *nspi = spi_controller_get_devdata(spi->controller);

	if (spi_get_csgpiod(spi, 0))
		gpiod_set_value(spi_get_csgpiod(spi, 0), 1);
	else
		writel(BIT(spi_get_chipselect(spi, 0)), nspi->base + REG_SSCTL);
}

static void nuc980_spi_cs_deassert(struct spi_device *spi)
{
	struct nuc980_spi *nspi = spi_controller_get_devdata(spi->controller);

	if (spi_get_csgpiod(spi, 0))
		gpiod_set_value(spi_get_csgpiod(spi, 0), 0);
	else
		writel(0x00, nspi->base + REG_SSCTL);
}

static int nuc980_spi_transfer_one_message(struct spi_controller *master,
						struct spi_message *m)
{
	struct nuc980_spi *nspi = spi_controller_get_devdata(master);
	struct spi_device *spi = m->spi;
	struct spi_transfer *t = NULL;
	unsigned long speed_hz;
	int cs_change = 1;
	int spimode;
	int i;
	u8 data;
	u32 clkdiv;
	u32 ctl = (8 << 8) | CTL_SPIEN; // data bits
	u32 status;
	int ret;

	writel(0x00, nspi->base + REG_CTL);

	speed_hz = min(nspi->rate, (unsigned long)spi->max_speed_hz);

	list_for_each_entry(t, &m->transfers, transfer_list)
		if (t->speed_hz < speed_hz)
			speed_hz = t->speed_hz;

	if (!speed_hz)
		speed_hz = 100000;

	ret = readl_poll_timeout(nspi->base + REG_STATUS, status,
				 !(status & STATUS_ENSTS), 0,
				 NUC980_SPI_CTL_TIMEOUT_US);
	if (ret) {
		dev_err(&spi->dev, "timeout waiting for SPI disable\n");
		m->status = ret;
		spi_finalize_current_message(master);
		return 0;
	}

	clkdiv = min(DIV_ROUND_UP(nspi->rate, speed_hz), 0x200LU) - 1;

	writel(clkdiv, nspi->base + REG_CLKDIV);

	/*
	 * SPI mode to NUC980 register mapping (verified against TRM):
	 *   CLKPOL = idle clock level (0=low, 1=high)
	 *   TXNEG  = transmit on falling edge
	 *   RXNEG  = receive/sample on falling edge
	 *   TXNEG and RXNEG are mutually exclusive per TRM.
	 */
	spimode = spi->mode & (SPI_CPOL | SPI_CPHA);
	if (spimode == SPI_MODE_0) {
		ctl |= CTL_TXNEG;
	} else if (spimode == SPI_MODE_1) {
		ctl |= CTL_RXNEG;
	} else if (spimode == SPI_MODE_2) {
		ctl |= CTL_CLKPOL | CTL_RXNEG;
	} else if (spimode == SPI_MODE_3) {
		ctl |= CTL_CLKPOL | CTL_TXNEG;
	}

	if (spi->mode & SPI_LSB_FIRST)
		ctl |= CTL_LSB;

	nuc980_spi_cs_assert(spi);
	writel(ctl, nspi->base + REG_CTL);

	ret = readl_poll_timeout(nspi->base + REG_STATUS, status,
				 status & STATUS_ENSTS, 0,
				 NUC980_SPI_CTL_TIMEOUT_US);
	if (ret) {
		dev_err(&spi->dev, "timeout waiting for SPI enable\n");
		goto err_out;
	}

	m->actual_length = 0;

	list_for_each_entry(t, &m->transfers, transfer_list) {

		if (cs_change)
			nuc980_spi_cs_assert(spi);

		cs_change = t->cs_change;

		writel(FIFOCTL_RXRST | FIFOCTL_TXRST, nspi->base + REG_FIFOCTL);
		ret = readl_poll_timeout(nspi->base + REG_STATUS, status,
					 !(status & STATUS_TXRXRST), 0,
					 NUC980_SPI_CTL_TIMEOUT_US);
		if (ret) {
			dev_err(&spi->dev, "timeout waiting for FIFO reset\n");
			goto err_out;
		}

		ctl &= ~(CTL_DATDIR | CTL_DUALIOEN | CTL_QUADIOEN);
		if (t->rx_nbits & SPI_NBITS_DUAL)
			ctl |= CTL_DUALIOEN;
		else if (t->rx_nbits & SPI_NBITS_QUAD)
			ctl |= CTL_QUADIOEN;
		else if (t->tx_nbits & SPI_NBITS_DUAL)
			ctl |= CTL_DUALIOEN | CTL_DATDIR;
		else if (t->tx_nbits & SPI_NBITS_QUAD)
			ctl |= CTL_QUADIOEN | CTL_DATDIR;

		writel(ctl, nspi->base + REG_CTL);

		reinit_completion(&nspi->done);

		nspi->rx_buf = t->rx_buf;
		nspi->tx_buf = t->tx_buf;
		nspi->tx_len = t->len;
		nspi->rx_len = t->len;

		for (i = 0; i < nspi->threshold && nspi->tx_len; i++) {
			data = nspi->tx_buf ? *nspi->tx_buf++ : 0;
			writel(data, nspi->base + REG_TX);
			nspi->tx_len--;
		}

		if (i >= nspi->threshold) {
			unsigned long timeout;

			writel(FIFOCTL_RXTHIEN | ((nspi->threshold - 1) << 24),
						nspi->base + REG_FIFOCTL);

			timeout = wait_for_completion_timeout(&nspi->done,
				msecs_to_jiffies(NUC980_SPI_XFER_TIMEOUT_MS));
			if (!timeout) {
				writel(0x00, nspi->base + REG_FIFOCTL);
				dev_err(&spi->dev, "SPI transfer timeout\n");
				ret = -ETIMEDOUT;
				goto err_out;
			}
		}

		while (nspi->rx_len) {
			ret = readl_poll_timeout(nspi->base + REG_STATUS,
						 status,
						 !(status & STATUS_RXEMPTY),
						 0, NUC980_SPI_RX_TIMEOUT_US);
			if (ret) {
				dev_err(&spi->dev,
					"timeout waiting for RX data\n");
				goto err_out;
			}

			data = readl(nspi->base + REG_RX);
			if (nspi->rx_buf)
				*nspi->rx_buf++ = data;

			nspi->rx_len--;
		}

		m->actual_length += t->len;

		if (cs_change)
			nuc980_spi_cs_deassert(spi);
	}

	if (!cs_change)
		nuc980_spi_cs_deassert(spi);

	writel(0x00, nspi->base + REG_FIFOCTL);

	m->status = 0;

	spi_finalize_current_message(master);
	return 0;

err_out:
	nuc980_spi_cs_deassert(spi);
	writel(0x00, nspi->base + REG_FIFOCTL);
	writel(0x00, nspi->base + REG_CTL);
	m->status = ret;
	spi_finalize_current_message(master);
	return 0;
}

static irqreturn_t nuc980_spi_irq(int irq, void *dev_id)
{
	struct nuc980_spi *nspi = dev_id;
	u8 data;
	int i;

	while (!(readl(nspi->base + REG_STATUS) & STATUS_RXEMPTY)) {
		data = readl(nspi->base + REG_RX);
		if (nspi->rx_buf)
			*nspi->rx_buf++ = data;

		if (--nspi->rx_len <= 0) {
			writel(0x00, nspi->base + REG_FIFOCTL);
			complete(&nspi->done);
			return IRQ_HANDLED;
		}
	}

	for (i = 0; i < nspi->threshold && nspi->tx_len; i++) {
		data = nspi->tx_buf ? *nspi->tx_buf++ : 0;
		writel(data, nspi->base + REG_TX);
		nspi->tx_len--;
	}

	if (i < nspi->threshold) {
		writel(0x00, nspi->base + REG_FIFOCTL);
		complete(&nspi->done);
	}

	return IRQ_HANDLED;
}

static int nuc980_spi_probe(struct platform_device *pdev)
{
	struct nuc980_spi *nspi;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct spi_controller *master;
	struct reset_control *rst;
	struct resource res;
	u32 fifo_size;
	u32 num_cs;
	u32 status;
	int irq;
	int ret;

	master = spi_alloc_master(dev, sizeof(*nspi));
	if (!master) {
		dev_err(dev, "unable to allocate memory\n");
		return -ENOMEM;
	}

	nspi = spi_controller_get_devdata(master);
	platform_set_drvdata(pdev, nspi);
	nspi->master = master;

	ret = of_address_to_resource(np, 0, &res);
	if (ret) {
		dev_err(dev, "unable to get mem region\n");
		goto put_master;
	}

	if (!res.start || resource_size(&res) < NUC980_SPI_REG_SIZE) {
		dev_err(dev, "mem region out of range\n");
		ret = -EINVAL;
		goto put_master;
	}

	dev_set_name(dev, "%08x.nuc980-spi", (unsigned int)res.start);

	nspi->eclk = devm_clk_get(dev, "eclk");
	if (IS_ERR(nspi->eclk)) {
		dev_err(dev, "unable to get clock: eclk\n");
		ret = PTR_ERR(nspi->eclk);
		goto put_master;
	}

	nspi->pclk = devm_clk_get(dev, "pclk");
	if (IS_ERR(nspi->pclk)) {
		dev_warn(dev, "unable to get clock: pclk\n");
		nspi->pclk = NULL;
	}

	ret = clk_prepare_enable(nspi->eclk);
	if (ret) {
		dev_err(dev, "unable to enable clock: eclk\n");
		goto put_master;
	}

	ret = clk_prepare_enable(nspi->pclk);
	if (ret) {
		dev_err(dev, "unable to enable clock: pclk\n");
		goto disable_eclk;
	}

	rst = devm_reset_control_get_optional_shared(dev, NULL);
	if (!IS_ERR(rst))
		reset_control_deassert(rst);

	nspi->base = devm_ioremap_resource(dev, &res);
	if (IS_ERR(nspi->base)) {
		dev_err(dev, "unable to map mem region\n");
		ret = PTR_ERR(nspi->base);
		goto disable_pclk;
	}

	writel(0x00, nspi->base + REG_CTL);
	ret = readl_poll_timeout(nspi->base + REG_STATUS, status,
				 !(status & STATUS_ENSTS), 0,
				 NUC980_SPI_CTL_TIMEOUT_US);
	if (ret) {
		dev_err(dev, "timeout waiting for SPI disable\n");
		goto disable_pclk;
	}

	irq = irq_of_parse_and_map(np, 0);
	if (irq <= 0) {
		dev_err(dev, "unable to get irq\n");
		ret = -EINVAL;
		goto disable_pclk;
	}

	ret = devm_request_irq(dev, irq, nuc980_spi_irq,
					0, dev_name(dev), nspi);

	if (ret) {
		dev_err(dev, "unable to request irq\n");
		goto disable_pclk;
	}

	init_completion(&nspi->done);

	nspi->rate = clk_get_rate(nspi->eclk);

	master->use_gpio_descriptors = true;
	master->mode_bits = SPI_CPHA | SPI_CPOL | SPI_CS_HIGH | SPI_LSB_FIRST |
			SPI_TX_DUAL | SPI_RX_DUAL | SPI_TX_QUAD | SPI_RX_QUAD;
	master->transfer_one_message = nuc980_spi_transfer_one_message;
	master->bits_per_word_mask = SPI_BPW_MASK(8);
	master->dev.of_node = np;

	if (of_property_read_u32(np, "num-cs", &num_cs) ||
					num_cs > NUC980_SPI_MAX_NUM_CS)
		master->num_chipselect = NUC980_SPI_MAX_NUM_CS;
	else
		master->num_chipselect = num_cs;

	if (of_property_read_u32(np, "nuvoton,fifo-size", &fifo_size))
		nspi->threshold = 4;
	else
		nspi->threshold = max(min(fifo_size, (u32)8), (u32)1);

	writel(0x00, nspi->base + REG_PDMACTL);
	writel(FIFOCTL_RXRST | FIFOCTL_TXRST, nspi->base + REG_FIFOCTL);
	ret = readl_poll_timeout(nspi->base + REG_STATUS, status,
				 !(status & STATUS_TXRXRST), 0,
				 NUC980_SPI_CTL_TIMEOUT_US);
	if (ret) {
		dev_err(dev, "timeout waiting for FIFO reset\n");
		goto disable_pclk;
	}

	writel(0x1FF, nspi->base + REG_CLKDIV);

	ret = devm_spi_register_controller(dev, nspi->master);
	if (ret) {
		dev_err(dev, "unable to register spi controller\n");
		goto disable_pclk;
	}

	dev_info(dev, "initialized\n");

	return 0;

disable_pclk:
	clk_disable_unprepare(nspi->pclk);
disable_eclk:
	clk_disable_unprepare(nspi->eclk);
put_master:
	spi_controller_put(master);
	return ret;
}

static const struct of_device_id nuc980_spi_dt_ids[] = {
	{ .compatible = "nuvoton,nuc980-spi" },
	{},
};
MODULE_DEVICE_TABLE(of, nuc980_spi_dt_ids);

static struct platform_driver nuc980_spi_driver = {
	.probe = nuc980_spi_probe,
	.driver = {
		.name = "nuc980-spi",
		.of_match_table = nuc980_spi_dt_ids,
	},
};

static int __init nuc980_spi_init(void)
{
	return platform_driver_register(&nuc980_spi_driver);
}
device_initcall(nuc980_spi_init);
