// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020, Veiko Rütter <rebane@alkohol.ee>
 *
 * Based on previous work from:
 * Copyright (C) 2018 Nuvoton Technology Corp.
 *
 */

#include <asm/byteorder.h>
#include <linux/bitops.h>
#include <linux/blkdev.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sd.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_dma.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/sched.h>
#include <linux/spinlock.h>

#define REG_FB0            0x0000
#define REG_FB1            0x0004
#define REG_FB2            0x0008
#define REG_FB3            0x000c
#define REG_FB4            0x0010
#define REG_DMACCSR        0x0400
#define   DMACCSR_DMACEN   BIT(0)
#define   DMACCSR_SWRST    BIT(1)
#define REG_DMACSAR        0x0408
#define REG_DMACBCR        0x040C
#define REG_DMACIER        0x0410
#define   DMACIER_TABORTIE BIT(0)
#define REG_DMACISR        0x0414

#define REG_FMICSR         0x0800
#define   FMICSR_SWRST     BIT(0)
#define   FMICSR_SDEN      BIT(1)
#define REG_FMIIER         0x0804
#define   FMIIER_DTAIE     BIT(0)
#define REG_FMIISR         0x0808

#define REG_SDCSR          0x0820
#define   SDCSR_COEN       BIT(0)
#define   SDCSR_RIEN       BIT(1)
#define   SDCSR_DIEN       BIT(2)
#define   SDCSR_DOEN       BIT(3)
#define   SDCSR_R2EN       BIT(4)
#define   SDCSR_CLK8_OE    BIT(6)
#define   SDCSR_CLK_KEEP0  BIT(7)
#define   SDCSR_CTLRST     BIT(14)
#define   SDCSR_DBW        BIT(15)
#define REG_SDARG          0x0824
#define REG_SDIER          0x0828
#define   SDIER_BLKDIE     BIT(0)
#define   SDIER_CD0IE      BIT(8)
#define   SDIER_SDIO0IE    BIT(10)
#define   SDIER_RITOIE     BIT(12)
#define   SDIER_DITOIE     BIT(13)
#define   SDIER_WKUPIE     BIT(14)
#define   SDIER_DONEIE     BIT(24)
#define   SDIER_CD0SRC     BIT(30)
#define REG_SDISR          0x082C
#define   SDISR_BLKDIF     BIT(0)
#define   SDISR_CRCIF      BIT(1)
#define   SDISR_CRC7       BIT(2)
#define   SDISR_SDDAT0     BIT(7)
#define   SDISR_CD0IF      BIT(8)
#define   SDISR_SDIO0IF    BIT(10)
#define   SDISR_RITOIF     BIT(12)
#define   SDISR_DITOIF     BIT(13)
#define   SDISR_CDPS0      BIT(16)
#define   SDISR_DONEIF     BIT(24)
#define REG_SDRSP0         0x0830
#define REG_SDRSP1         0x0834
#define REG_SDBLEN         0x0838
#define REG_SDTOUT         0x083C
#define REG_SDECR          0x0840
#define NUC980_SD_REG_SIZE 0x1000

struct nuc980_sd {
	void __iomem         *base;
	struct clk           *hclk;
	struct clk           *eclk;
	struct clk           *fmiclk;
	struct reset_control *rst;
	int                  irq;
	struct device        *dev;
	struct mmc_host      *mmc;
	struct mmc_host_ops  host_ops;
	dma_addr_t           paddr;
	unsigned int         *buffer;
	int                  bus_width;
	bool                 cd;
	volatile bool        active;
	volatile bool        changed;
	struct completion    done;
};

static void nuc980_sd_dma_write(struct nuc980_sd *nsd, struct mmc_command *cmd)
{
	struct mmc_data *data = cmd->data;
	unsigned *dmabuf = nsd->buffer;
	unsigned size = data->blksz * data->blocks;
	unsigned len = data->sg_len;
	unsigned i;

	for (i = 0; i < len; i++) {
		struct scatterlist *sg;
		int amount;
		unsigned int *sgbuffer;

		sg = &data->sg[i];

		sgbuffer = kmap_atomic(sg_page(sg)) + sg->offset;
		amount = min(size, sg->length);
		size -= amount;
		{
			char *tmpv = (char *)dmabuf;
			memcpy(tmpv, sgbuffer, amount);
			tmpv += amount;
			dmabuf = (unsigned *)tmpv;
		}
		kunmap_atomic(sgbuffer);
		data->bytes_xfered += amount;
		if (size == 0)
			break;
	}
}

static void nuc980_sd_dma_read(struct nuc980_sd *nsd, struct mmc_command *cmd)
{
	struct mmc_data *data = cmd->data;
	unsigned *dmabuf = nsd->buffer;
	unsigned size = data->blksz * data->blocks;
	unsigned len = data->sg_len;
	unsigned i;

	for (i = 0; i < len; i++) {
		struct scatterlist *sg;
		int amount;
		unsigned int *sgbuffer;

		sg = &data->sg[i];

		sgbuffer = kmap_atomic(sg_page(sg)) + sg->offset;
		amount = min(size, sg->length);
		size -= amount;
		{
			char *tmpv = (char *)dmabuf;
			memcpy(sgbuffer, tmpv, amount);
			tmpv += amount;
			dmabuf = (unsigned *)tmpv;
		}
		flush_dcache_page(sg_page(sg));
		kunmap_atomic(sgbuffer);
		data->bytes_xfered += amount;
		if (size == 0)
			break;
	}
}

static void nuc980_sd_command(struct nuc980_sd *nsd, struct mmc_command *cmd)
{
	struct mmc_data *data = cmd->data;
	bool wait_r2 = false;
	bool wait_ri = false;
	u32 ier;
	u32 csr;
	u32 isr;
	int ret;
	int i;

	ier = readl(nsd->base + REG_SDIER) & SDIER_SDIO0IE;
	if (nsd->cd)
		ier |= SDIER_CD0SRC | SDIER_CD0IE;

	csr = readl(nsd->base + REG_SDCSR) & SDCSR_CLK_KEEP0;
	csr |= 0x09010000 | ((cmd->opcode & 0x3f) << 8) | SDCSR_COEN;

	if (nsd->bus_width == MMC_BUS_WIDTH_4)
		csr |= SDCSR_DBW;

	if (mmc_resp_type(cmd) == MMC_RSP_R2) {
		wait_r2 = true;
		csr |= SDCSR_R2EN;
		writel(0xffff, nsd->base + REG_SDTOUT);
	} else if (mmc_resp_type(cmd) != MMC_RSP_NONE) {
		wait_ri = true;
		ier |= SDIER_DONEIE;
		csr |= SDCSR_RIEN;
		writel(0xffff, nsd->base + REG_SDTOUT);
	}

	if (data) {
		writel(data->blksz - 1, nsd->base + REG_SDBLEN);
		ier &= ~SDIER_DONEIE;
		ier |= SDIER_BLKDIE | SDIER_DITOIE;
		csr = (csr & 0xff00ffff) | ((data->blocks & 0xff) << 16);

		data->bytes_xfered = 0;
		writel(nsd->paddr, nsd->base + REG_DMACSAR);
		if (data->flags & MMC_DATA_READ) {
			writel(0x3fffff, nsd->base + REG_SDTOUT);
			csr |= SDCSR_DIEN;
		} else if (data->flags & MMC_DATA_WRITE) {
			nuc980_sd_dma_write(nsd, cmd);
			csr |= SDCSR_DOEN;
		}
	}

	writel(0xffffffff, nsd->base + REG_SDISR);
	writel(ier, nsd->base + REG_SDIER);

	reinit_completion(&nsd->done);

	writel(cmd->arg, nsd->base + REG_SDARG);
	writel(csr, nsd->base + REG_SDCSR);

	if ((mmc_resp_type(cmd) == MMC_RSP_NONE) && !data) {
		while (readl(nsd->base + REG_SDCSR) & SDCSR_COEN)
			cpu_relax();
		return;
	}

	for (i = 0; i < 20; i++) {
		ret = wait_for_completion_interruptible_timeout(&nsd->done,
							msecs_to_jiffies(50));

		if (ret < 0) {
			writel(readl(nsd->base + REG_SDIER) & ~(SDIER_BLKDIE |
				SDIER_RITOIE | SDIER_DITOIE | SDIER_DONEIE),
				nsd->base + REG_SDIER);
			cmd->error = ret;
			break;
		}

		if (nsd->changed) {
			cmd->error = -ETIMEDOUT;
			break;
		}

		isr = readl(nsd->base + REG_SDISR);
		if (isr & (SDISR_RITOIF | SDISR_DITOIF)) {
			cmd->error = -ETIMEDOUT;
			break;
		}

		if (wait_r2 && !(readl(nsd->base + REG_SDCSR) & SDCSR_R2EN)) {
			cmd->resp[0] =
				(cpu_to_be32(readl(nsd->base + REG_FB0)) << 8) |
				(cpu_to_be32(readl(nsd->base + REG_FB1)) >> 24);
			cmd->resp[1] =
				(cpu_to_be32(readl(nsd->base + REG_FB1)) << 8) |
				(cpu_to_be32(readl(nsd->base + REG_FB2)) >> 24);
			cmd->resp[2] =
				(cpu_to_be32(readl(nsd->base + REG_FB2)) << 8) |
				(cpu_to_be32(readl(nsd->base + REG_FB3)) >> 24);
			cmd->resp[3] =
				(cpu_to_be32(readl(nsd->base + REG_FB3)) << 8) |
				(cpu_to_be32(readl(nsd->base + REG_FB4)) >> 24);

			if (!data)
				return;

			wait_r2 = false;
		}

		if (wait_ri && !(readl(nsd->base + REG_SDCSR) & SDCSR_RIEN)) {
			cmd->resp[0] = (readl(nsd->base + REG_SDRSP0) << 8) |
				(readl(nsd->base + REG_SDRSP1) & 0xff);
			cmd->resp[1] = cmd->resp[2] = cmd->resp[3] = 0;

			if (!data)
				return;

			wait_ri = false;
		}

//		if (data && (data->flags & MMC_DATA_READ)) {
			if (data && (isr & SDISR_BLKDIF)) {
				if (!(isr & SDISR_CRC7) &&
					(mmc_resp_type(cmd) & MMC_RSP_CRC)) {
					cmd->error = -EIO;
					break;
				}

				if (data->flags & MMC_DATA_READ)
					nuc980_sd_dma_read(nsd, cmd);

				return;
			}
/*		} else if (data && (data->flags & MMC_DATA_WRITE)) {
			// if (isr & SDISR_DONEIF) {
			if ((isr & SDISR_BLKDIF) || (isr & SDISR_DONEIF))
				return;
		}*/
	}

/*	if (data && (data->flags & MMC_DATA_WRITE))
		return;

	if (!cmd->error)
		cmd->error = -ETIMEDOUT;
*/
	writel(DMACCSR_DMACEN | DMACCSR_SWRST, nsd->base + REG_DMACCSR);
	writel(SDCSR_CTLRST, nsd->base + REG_SDCSR);

	while(readl(nsd->base + REG_DMACCSR) & DMACCSR_SWRST)
		cpu_relax();

	while(readl(nsd->base + REG_SDCSR) & SDCSR_CTLRST)
		cpu_relax();

	cmd->resp[0] = cmd->resp[1] = cmd->resp[2] = cmd->resp[3] = 0;
}

static irqreturn_t nuc980_sd_irq(int irq, void *dev_id)
{
	struct nuc980_sd *nsd = dev_id;
	u32 flags = (SDIER_BLKDIE | SDIER_RITOIE | SDIER_DITOIE | SDIER_DONEIE);
	u32 isr;

	isr = readl(nsd->base + REG_SDISR);

	if (isr & SDISR_SDIO0IF) {
		writel(SDISR_SDIO0IF, nsd->base + REG_SDISR);
                mmc_signal_sdio_irq(nsd->mmc);
	}

	if (isr & SDISR_CD0IF) {
		writel(SDISR_CD0IF, nsd->base + REG_SDISR);
		if (nsd->cd) {
			if (nsd->active) {
				writel(readl(nsd->base + REG_SDIER) & ~flags,
						nsd->base + REG_SDIER);
				nsd->changed = true;
				complete(&nsd->done);
			}

			mmc_detect_change(nsd->mmc, msecs_to_jiffies(500));
		}
	}

	if (isr & (SDISR_BLKDIF | SDISR_RITOIF | SDISR_DITOIF | SDISR_DONEIF)) {
		writel(readl(nsd->base + REG_SDIER) & ~flags,
						nsd->base + REG_SDIER);

		if (nsd->active)
			complete(&nsd->done);
	}

	return IRQ_HANDLED;
}

static void nuc980_sd_request(struct mmc_host *mmc, struct mmc_request *req)
{
	struct nuc980_sd *nsd = mmc_priv(mmc);

	nsd->changed = false;
	nsd->active = true;

	nuc980_sd_command(nsd, req->cmd);

	if (req->stop) {
		if (nsd->changed)
			req->stop->error = -ETIMEDOUT;
		else
			nuc980_sd_command(nsd, req->stop);
	}

	nsd->active = false;
	mmc_request_done(nsd->mmc, req);
}

static void nuc980_sd_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct nuc980_sd *nsd = mmc_priv(mmc);

	nsd->bus_width = ios->bus_width;

	switch (ios->power_mode) {
	case MMC_POWER_OFF:
		writel(0x00, nsd->base + REG_FMICSR);
		writel(0x01, nsd->base + REG_SDECR);
		break;
	case MMC_POWER_UP:
	case MMC_POWER_ON:
		writel(0x00, nsd->base + REG_SDECR);
		writel(FMICSR_SDEN, nsd->base + REG_FMICSR);

		if (ios->clock == 0)
			break;

		clk_set_rate(nsd->eclk, ios->clock);
	}
}

static int nuc980_sd_card_detect(struct mmc_host *mmc)
{
	struct nuc980_sd *nsd = mmc_priv(mmc);

	return !(readl(nsd->base + REG_SDISR) & SDISR_CDPS0);
}

static void nuc980_sd_enable_sdio_irq(struct mmc_host *mmc, int enable)
{
	struct nuc980_sd *nsd = mmc_priv(mmc);

	if (enable) {
		writel(readl(nsd->base + REG_SDIER) | SDIER_SDIO0IE,
						nsd->base + REG_SDIER);
		writel(readl(nsd->base + REG_SDCSR) | SDCSR_CLK_KEEP0,
						nsd->base + REG_SDCSR);
	} else {
		writel(readl(nsd->base + REG_SDIER) & ~SDIER_SDIO0IE,
						nsd->base + REG_SDIER);
		writel(readl(nsd->base + REG_SDCSR) & ~SDCSR_CLK_KEEP0,
						nsd->base + REG_SDCSR);
	}
}

static int nuc980_sd_probe(struct platform_device *pdev)
{
	struct mmc_host *mmc;
	struct nuc980_sd *nsd;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct resource res;
	int ret;

	mmc = mmc_alloc_host(sizeof(*nsd), dev);
	if (!mmc) {
		dev_err(dev, "unable to allocate memory\n");
		return -ENOMEM;
	}

	nsd = mmc_priv(mmc);
        nsd->mmc = mmc;
	nsd->dev = dev;

	platform_set_drvdata(pdev, nsd);

	ret = of_address_to_resource(np, 0, &res);
	if (ret) {
		dev_err(dev, "unable to get mem region\n");
		goto free_mmc;
	}

	if (!res.start || resource_size(&res) < NUC980_SD_REG_SIZE) {
		dev_err(dev, "mem region out of range\n");
		ret = -EINVAL;
		goto free_mmc;
	}

	dev_set_name(dev, "%08x.nuc980-sd", (unsigned int)res.start);

	nsd->eclk = devm_clk_get(dev, "eclk");
	if (IS_ERR(nsd->eclk)) {
		dev_err(dev, "unable to get clock: eclk\n");
		ret = PTR_ERR(nsd->eclk);
		goto free_mmc;
	}

	nsd->hclk = devm_clk_get(dev, "hclk");
	if (IS_ERR(nsd->hclk)) {
		dev_warn(dev, "unable to get clock: hclk\n");
		nsd->hclk = NULL;
	}

	nsd->fmiclk = devm_clk_get(dev, "fmiclk");
	if (IS_ERR(nsd->fmiclk))
		nsd->fmiclk = NULL;

	ret = clk_prepare_enable(nsd->fmiclk);
	if (ret) {
		dev_err(dev, "unable to enable clock: fmiclk\n");
		goto free_mmc;
	}

	ret = clk_prepare_enable(nsd->eclk);
	if (ret) {
		dev_err(dev, "unable to enable clock: eclk\n");
		goto disable_fmiclk;
	}

	ret = clk_prepare_enable(nsd->hclk);
	if (ret) {
		dev_err(dev, "unable to enable clock: hclk\n");
		goto disable_eclk;
	}

	nsd->rst = devm_reset_control_get_optional_shared(dev, NULL);
	if (!IS_ERR(nsd->rst))
		reset_control_deassert(nsd->rst);

	nsd->base = devm_ioremap_resource(dev, &res);
	if (IS_ERR(nsd->base)) {
		dev_err(dev, "unable to map mem region\n");
		ret = PTR_ERR(nsd->base);
		goto disable_hclk;
	}

	nsd->irq = irq_of_parse_and_map(np, 0);
	if (nsd->irq <= 0) {
		dev_err(dev, "unable to get irq\n");
		ret = -EINVAL;
		goto disable_hclk;
	}

	ret = devm_request_irq(dev, nsd->irq, nuc980_sd_irq,
					0, dev_name(dev), nsd);

	if (ret) {
		dev_err(dev, "unable to request irq\n");
		goto disable_hclk;
	}

	nsd->buffer = dma_alloc_coherent(dev, (512 * 255), &nsd->paddr,
								GFP_KERNEL);
	if (!nsd->buffer) {
		dev_err(dev, "unable to allocate buffer\n");
		ret = -ENOMEM;
		goto disable_hclk;
	}

	init_completion(&nsd->done);

	mmc->f_max         = 50000000;
	mmc->f_min         = 300000;

	mmc_of_parse(mmc);
	mmc->caps          &= (MMC_CAP_4_BIT_DATA | MMC_CAP_SDIO_IRQ |
				MMC_CAP_SD_HIGHSPEED | MMC_CAP_MMC_HIGHSPEED |
				MMC_CAP_NONREMOVABLE | MMC_CAP_NEEDS_POLL);

	mmc->ops           = &nsd->host_ops;
	mmc->ocr_avail     = MMC_VDD_27_28 | MMC_VDD_28_29 | MMC_VDD_29_30 |
				MMC_VDD_30_31 | MMC_VDD_31_32 | MMC_VDD_32_33 |
				MMC_VDD_33_34;

	mmc->max_blk_size  = 512;
	mmc->max_blk_count = 255;
	mmc->max_req_size  = (512 * 255);
	mmc->max_segs      = 255;
	mmc->max_seg_size  = (512 * 255);

	nsd->host_ops.request = nuc980_sd_request;
	nsd->host_ops.set_ios = nuc980_sd_set_ios;
	nsd->host_ops.enable_sdio_irq = nuc980_sd_enable_sdio_irq;

	if (!(mmc->caps & (MMC_CAP_NONREMOVABLE | MMC_CAP_NEEDS_POLL)) &&
			!of_property_read_bool(np, "cd-gpios")) {
		nsd->host_ops.get_cd = nuc980_sd_card_detect;
		nsd->cd = true;
	}

	/* disable */
	writel(DMACCSR_SWRST, nsd->base + REG_DMACCSR);
	writel(FMICSR_SWRST, nsd->base + REG_FMICSR);
	writel(0xffffffff, nsd->base + REG_SDISR);

	/* enable */
	writel(DMACCSR_DMACEN, nsd->base + REG_DMACCSR);
	writel(FMICSR_SDEN, nsd->base + REG_FMICSR);
	writel(0xffffffff, nsd->base + REG_SDISR);

	if (nsd->cd)
		writel(SDIER_CD0SRC | SDIER_CD0IE, nsd->base + REG_SDIER);
	else
		writel(0x00, nsd->base + REG_SDIER);

	writel(0x09010000, nsd->base + REG_SDCSR);

	ret = mmc_add_host(mmc);
	if (ret) {
		dev_err(dev, "unable to add mmc host\n");
		goto free_dma;
	}

	dev_info(dev, "initialized\n");

	return 0;

free_dma:
	dma_free_coherent(dev, (512 * 255), nsd->buffer, nsd->paddr);
disable_hclk:
	clk_disable_unprepare(nsd->hclk);
disable_eclk:
	clk_disable_unprepare(nsd->eclk);
disable_fmiclk:
	clk_disable_unprepare(nsd->fmiclk);
free_mmc:
	mmc_free_host(mmc);
	return ret;
}

static const struct of_device_id nuc980_sd_dt_ids[] = {
	{ .compatible = "nuvoton,nuc980-sd" },
	{ }
};
MODULE_DEVICE_TABLE(of, nuc980_sd_dt_ids);

static struct platform_driver nuc980_sd_driver = {
	.probe      = nuc980_sd_probe,
	.driver     = {
		.name		= "nuc980-sd",
		.of_match_table	= nuc980_sd_dt_ids,
	},
};

static int __init nuc980_sd_init(void)
{
        return platform_driver_register(&nuc980_sd_driver);
}
device_initcall(nuc980_sd_init);
