// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton NUC980 PDMA Engine Driver
 *
 * Copyright (C) 2020, Veiko Rütter <rebane@alkohol.ee>
 *
 * Based on previous work from:
 * Copyright (C) 2018 Nuvoton Technology Corp.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/slab.h>

#include "dmaengine.h"
#include "virt-dma.h"

#define NUC980_PDMA_CHANNELS	10

/* Per-channel DSCT registers (channel N at base + N * 0x10) */
#define PDMA_DSCT_CTL(n)	(0x000 + (n) * 0x10)
#define PDMA_DSCT_SA(n)		(0x004 + (n) * 0x10)
#define PDMA_DSCT_DA(n)		(0x008 + (n) * 0x10)
#define PDMA_DSCT_NEXT(n)	(0x00C + (n) * 0x10)

/* Global registers (at base + 0x400) */
#define PDMA_CHCTL		0x400
#define PDMA_PAUSE		0x404
#define PDMA_SWREQ		0x408
#define PDMA_INTEN		0x418
#define PDMA_INTSTS		0x41C
#define PDMA_APTS		0x420
#define PDMA_TDSTS		0x424
#define PDMA_ALIGN		0x428
#define PDMA_TACTSTS		0x42C
#define PDMA_TOUTEN		0x434
#define PDMA_TOUTIEN		0x438
#define PDMA_TOC0_3		0x444
#define PDMA_TOC4_7		0x448
#define PDMA_REQSEL0		0x480
#define PDMA_REQSEL1		0x484
#define PDMA_REQSEL2		0x488

/* DSCT_CTL bit fields */
#define PDMA_CTL_OPMODE_IDLE	0x0
#define PDMA_CTL_OPMODE_BASIC	0x1
#define PDMA_CTL_TXTYPE_SINGLE	BIT(2)
#define PDMA_CTL_SAINC_SHIFT	8
#define PDMA_CTL_DAINC_SHIFT	10
#define PDMA_CTL_TXWIDTH_SHIFT	12
#define PDMA_CTL_TXCNT_SHIFT	16

/* SAINC/DAINC values */
#define PDMA_INC_ADDR		0x0
#define PDMA_INC_FIXED		0x3

/* TXWIDTH values */
#define PDMA_WIDTH_8		0x0
#define PDMA_WIDTH_16		0x1
#define PDMA_WIDTH_32		0x2

/* INTSTS bits */
#define PDMA_INTSTS_TDIF	BIT(1)

struct nuc980_dma_dev;

struct nuc980_dma_desc {
	struct virt_dma_desc	vdesc;
	dma_addr_t		src;
	dma_addr_t		dst;
	size_t			len;
	u32			ctl;
};

struct nuc980_dma_chan {
	struct virt_dma_chan	vchan;
	struct nuc980_dma_dev	*dev;
	int			id;
	bool			allocated;
	u32			reqsel;
	struct nuc980_dma_desc	*active;
	struct dma_slave_config	sconfig;
};

struct nuc980_dma_dev {
	void __iomem		*base;
	struct clk		*clk;
	struct reset_control	*rst;
	struct dma_device	ddev;
	struct nuc980_dma_chan	channels[NUC980_PDMA_CHANNELS];
	spinlock_t		lock;
};

static inline struct nuc980_dma_desc *to_nuc980_desc(struct virt_dma_desc *vd)
{
	return container_of(vd, struct nuc980_dma_desc, vdesc);
}

static inline struct nuc980_dma_chan *to_nuc980_chan(struct dma_chan *chan)
{
	return container_of(chan, struct nuc980_dma_chan, vchan.chan);
}

static void nuc980_dma_desc_free(struct virt_dma_desc *vd)
{
	kfree(to_nuc980_desc(vd));
}

/* --- REQSEL register helpers --- */

static void nuc980_dma_set_reqsel(struct nuc980_dma_dev *dmadev,
				  int ch, u32 reqsel)
{
	u32 reg_offset, val;
	int shift;

	/* REQSEL0: ch0-3, REQSEL1: ch4-7, REQSEL2: ch8-9 */
	reg_offset = PDMA_REQSEL0 + (ch / 4) * 4;
	shift = (ch % 4) * 8;

	val = readl(dmadev->base + reg_offset);
	val &= ~(0xFF << shift);
	val |= (reqsel & 0xFF) << shift;
	writel(val, dmadev->base + reg_offset);
}

/* --- Hardware programming --- */

static void nuc980_dma_start(struct nuc980_dma_chan *nchan)
{
	struct nuc980_dma_dev *dmadev = nchan->dev;
	struct nuc980_dma_desc *desc;
	struct virt_dma_desc *vd;
	int ch = nchan->id;

	vd = vchan_next_desc(&nchan->vchan);
	if (!vd) {
		nchan->active = NULL;
		return;
	}

	list_del(&vd->node);
	desc = to_nuc980_desc(vd);
	nchan->active = desc;

	/* Set REQSEL for this channel */
	nuc980_dma_set_reqsel(dmadev, ch, nchan->reqsel);

	/* Program DSCT registers */
	writel(desc->src, dmadev->base + PDMA_DSCT_SA(ch));
	writel(desc->dst, dmadev->base + PDMA_DSCT_DA(ch));
	writel(desc->ctl, dmadev->base + PDMA_DSCT_CTL(ch));

	/* Enable channel interrupt */
	writel(readl(dmadev->base + PDMA_INTEN) | BIT(ch),
	       dmadev->base + PDMA_INTEN);

	/* Enable channel — start transfer */
	writel(readl(dmadev->base + PDMA_CHCTL) | BIT(ch),
	       dmadev->base + PDMA_CHCTL);
}

static void nuc980_dma_stop(struct nuc980_dma_chan *nchan)
{
	struct nuc980_dma_dev *dmadev = nchan->dev;
	int ch = nchan->id;
	u32 val;

	/* Disable channel */
	val = readl(dmadev->base + PDMA_CHCTL);
	val &= ~BIT(ch);
	writel(val, dmadev->base + PDMA_CHCTL);

	/* Disable channel interrupt */
	val = readl(dmadev->base + PDMA_INTEN);
	val &= ~BIT(ch);
	writel(val, dmadev->base + PDMA_INTEN);

	/* Set channel to idle mode */
	writel(PDMA_CTL_OPMODE_IDLE, dmadev->base + PDMA_DSCT_CTL(ch));
}

/* --- DMA engine callbacks --- */

static int nuc980_dma_slave_config(struct dma_chan *chan,
				   struct dma_slave_config *config)
{
	struct nuc980_dma_chan *nchan = to_nuc980_chan(chan);

	memcpy(&nchan->sconfig, config, sizeof(*config));
	return 0;
}

static int nuc980_dma_alloc_chan_resources(struct dma_chan *chan)
{
	return 0;
}

static void nuc980_dma_free_chan_resources(struct dma_chan *chan)
{
	vchan_free_chan_resources(to_virt_chan(chan));
}

static struct dma_async_tx_descriptor *
nuc980_dma_prep_slave_sg(struct dma_chan *chan, struct scatterlist *sgl,
			 u32 sg_len, enum dma_transfer_direction dir,
			 unsigned long flags, void *context)
{
	struct nuc980_dma_chan *nchan = to_nuc980_chan(chan);
	struct nuc980_dma_desc *desc;
	struct scatterlist *sg;
	dma_addr_t dev_addr;
	u32 ctl, width_val, txcnt;

	if (unlikely(!sg_len || !is_slave_direction(dir)))
		return NULL;

	/* Only support single-entry scatterlists (basic mode) */
	if (sg_len != 1)
		return NULL;

	sg = sgl;

	desc = kzalloc(sizeof(*desc), GFP_NOWAIT);
	if (!desc)
		return NULL;

	/* Determine transfer width and addresses */
	if (dir == DMA_MEM_TO_DEV) {
		dev_addr = nchan->sconfig.dst_addr;
		switch (nchan->sconfig.dst_addr_width) {
		case DMA_SLAVE_BUSWIDTH_2_BYTES:
			width_val = PDMA_WIDTH_16;
			break;
		case DMA_SLAVE_BUSWIDTH_4_BYTES:
			width_val = PDMA_WIDTH_32;
			break;
		default:
			width_val = PDMA_WIDTH_8;
			break;
		}
		desc->src = sg_dma_address(sg);
		desc->dst = dev_addr;
	} else {
		dev_addr = nchan->sconfig.src_addr;
		switch (nchan->sconfig.src_addr_width) {
		case DMA_SLAVE_BUSWIDTH_2_BYTES:
			width_val = PDMA_WIDTH_16;
			break;
		case DMA_SLAVE_BUSWIDTH_4_BYTES:
			width_val = PDMA_WIDTH_32;
			break;
		default:
			width_val = PDMA_WIDTH_8;
			break;
		}
		desc->src = dev_addr;
		desc->dst = sg_dma_address(sg);
	}

	desc->len = sg_dma_len(sg);

	/* TXCNT is transfer count in units (bytes/halfwords/words) minus 1 */
	switch (width_val) {
	case PDMA_WIDTH_32:
		txcnt = desc->len / 4;
		break;
	case PDMA_WIDTH_16:
		txcnt = desc->len / 2;
		break;
	default:
		txcnt = desc->len;
		break;
	}

	/* Build DSCT_CTL value */
	ctl = PDMA_CTL_OPMODE_BASIC | PDMA_CTL_TXTYPE_SINGLE;
	ctl |= (width_val << PDMA_CTL_TXWIDTH_SHIFT);

	if (dir == DMA_MEM_TO_DEV) {
		/* Source increments (memory), dest fixed (peripheral) */
		ctl |= (PDMA_INC_ADDR << PDMA_CTL_SAINC_SHIFT);
		ctl |= (PDMA_INC_FIXED << PDMA_CTL_DAINC_SHIFT);
	} else {
		/* Source fixed (peripheral), dest increments (memory) */
		ctl |= (PDMA_INC_FIXED << PDMA_CTL_SAINC_SHIFT);
		ctl |= (PDMA_INC_ADDR << PDMA_CTL_DAINC_SHIFT);
	}

	ctl |= ((txcnt - 1) << PDMA_CTL_TXCNT_SHIFT);

	desc->ctl = ctl;

	return vchan_tx_prep(&nchan->vchan, &desc->vdesc, flags);
}

static void nuc980_dma_issue_pending(struct dma_chan *chan)
{
	struct nuc980_dma_chan *nchan = to_nuc980_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&nchan->vchan.lock, flags);
	if (vchan_issue_pending(&nchan->vchan) && !nchan->active)
		nuc980_dma_start(nchan);
	spin_unlock_irqrestore(&nchan->vchan.lock, flags);
}

static int nuc980_dma_terminate_all(struct dma_chan *chan)
{
	struct nuc980_dma_chan *nchan = to_nuc980_chan(chan);
	unsigned long flags;
	LIST_HEAD(head);

	spin_lock_irqsave(&nchan->vchan.lock, flags);
	nuc980_dma_stop(nchan);
	if (nchan->active) {
		vchan_terminate_vdesc(&nchan->active->vdesc);
		nchan->active = NULL;
	}
	vchan_get_all_descriptors(&nchan->vchan, &head);
	spin_unlock_irqrestore(&nchan->vchan.lock, flags);

	vchan_dma_desc_free_list(&nchan->vchan, &head);
	return 0;
}

static void nuc980_dma_synchronize(struct dma_chan *chan)
{
	struct nuc980_dma_chan *nchan = to_nuc980_chan(chan);

	vchan_synchronize(&nchan->vchan);
}

/* --- IRQ handler --- */

static irqreturn_t nuc980_dma_irq(int irq, void *data)
{
	struct nuc980_dma_dev *dmadev = data;
	u32 intsts, tdsts;
	int ch;

	intsts = readl(dmadev->base + PDMA_INTSTS);
	if (!(intsts & PDMA_INTSTS_TDIF))
		return IRQ_NONE;

	tdsts = readl(dmadev->base + PDMA_TDSTS);
	/* W1C: clear completed channel bits */
	writel(tdsts, dmadev->base + PDMA_TDSTS);
	/* W1C: clear transfer done interrupt flag */
	writel(PDMA_INTSTS_TDIF, dmadev->base + PDMA_INTSTS);

	for (ch = 0; ch < NUC980_PDMA_CHANNELS; ch++) {
		struct nuc980_dma_chan *nchan;

		if (!(tdsts & BIT(ch)))
			continue;

		nchan = &dmadev->channels[ch];
		spin_lock(&nchan->vchan.lock);
		if (nchan->active) {
			vchan_cookie_complete(&nchan->active->vdesc);
			nchan->active = NULL;
			nuc980_dma_start(nchan);
		}
		spin_unlock(&nchan->vchan.lock);
	}

	return IRQ_HANDLED;
}

/* --- DT xlate --- */

static struct dma_chan *nuc980_dma_xlate(struct of_phandle_args *dma_spec,
					 struct of_dma *ofdma)
{
	struct nuc980_dma_dev *dmadev = ofdma->of_dma_data;
	struct nuc980_dma_chan *nchan;
	u32 reqsel;
	int i;

	if (dma_spec->args_count != 1)
		return NULL;

	reqsel = dma_spec->args[0];

	spin_lock(&dmadev->lock);
	for (i = 0; i < NUC980_PDMA_CHANNELS; i++) {
		nchan = &dmadev->channels[i];
		if (!nchan->allocated) {
			nchan->allocated = true;
			nchan->reqsel = reqsel;
			spin_unlock(&dmadev->lock);
			return dma_get_slave_channel(&nchan->vchan.chan);
		}
	}
	spin_unlock(&dmadev->lock);

	return NULL;
}

/* --- Probe / init --- */

static int nuc980_dma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nuc980_dma_dev *dmadev;
	struct dma_device *ddev;
	int ret, irq, i;

	dmadev = devm_kzalloc(dev, sizeof(*dmadev), GFP_KERNEL);
	if (!dmadev)
		return -ENOMEM;

	dmadev->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dmadev->base))
		return PTR_ERR(dmadev->base);

	dmadev->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(dmadev->clk))
		return dev_err_probe(dev, PTR_ERR(dmadev->clk),
				     "failed to get clock\n");

	ret = clk_prepare_enable(dmadev->clk);
	if (ret)
		return ret;

	dmadev->rst = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(dmadev->rst)) {
		ret = PTR_ERR(dmadev->rst);
		goto disable_clk;
	}

	if (dmadev->rst) {
		reset_control_assert(dmadev->rst);
		udelay(10);
		reset_control_deassert(dmadev->rst);
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto disable_clk;
	}

	ret = devm_request_irq(dev, irq, nuc980_dma_irq, 0,
			       dev_name(dev), dmadev);
	if (ret)
		goto disable_clk;

	spin_lock_init(&dmadev->lock);

	/* Reset all channels */
	writel(0, dmadev->base + PDMA_CHCTL);
	writel(0, dmadev->base + PDMA_INTEN);
	writel(0xFFFF, dmadev->base + PDMA_TDSTS);
	writel(0xFFFF, dmadev->base + PDMA_INTSTS);

	/* Initialize DMA device */
	ddev = &dmadev->ddev;
	ddev->dev = dev;
	INIT_LIST_HEAD(&ddev->channels);

	dma_cap_zero(ddev->cap_mask);
	dma_cap_set(DMA_SLAVE, ddev->cap_mask);

	ddev->device_alloc_chan_resources = nuc980_dma_alloc_chan_resources;
	ddev->device_free_chan_resources = nuc980_dma_free_chan_resources;
	ddev->device_prep_slave_sg = nuc980_dma_prep_slave_sg;
	ddev->device_config = nuc980_dma_slave_config;
	ddev->device_issue_pending = nuc980_dma_issue_pending;
	ddev->device_tx_status = dma_cookie_status;
	ddev->device_terminate_all = nuc980_dma_terminate_all;
	ddev->device_synchronize = nuc980_dma_synchronize;

	ddev->src_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_1_BYTE) |
				BIT(DMA_SLAVE_BUSWIDTH_2_BYTES) |
				BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	ddev->dst_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_1_BYTE) |
				BIT(DMA_SLAVE_BUSWIDTH_2_BYTES) |
				BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	ddev->directions = BIT(DMA_DEV_TO_MEM) | BIT(DMA_MEM_TO_DEV);

	/* Initialize virtual channels */
	for (i = 0; i < NUC980_PDMA_CHANNELS; i++) {
		struct nuc980_dma_chan *nchan = &dmadev->channels[i];

		nchan->dev = dmadev;
		nchan->id = i;
		nchan->vchan.desc_free = nuc980_dma_desc_free;
		vchan_init(&nchan->vchan, ddev);
	}

	ret = dma_async_device_register(ddev);
	if (ret)
		goto disable_clk;

	ret = of_dma_controller_register(dev->of_node, nuc980_dma_xlate,
					 dmadev);
	if (ret)
		goto unregister_dma;

	platform_set_drvdata(pdev, dmadev);

	dev_info(dev, "NUC980 PDMA engine initialized (%d channels)\n",
		 NUC980_PDMA_CHANNELS);
	return 0;

unregister_dma:
	dma_async_device_unregister(ddev);
disable_clk:
	clk_disable_unprepare(dmadev->clk);
	return ret;
}

static const struct of_device_id nuc980_dma_dt_ids[] = {
	{ .compatible = "nuvoton,nuc980-pdma" },
	{},
};
MODULE_DEVICE_TABLE(of, nuc980_dma_dt_ids);

static struct platform_driver nuc980_dma_driver = {
	.probe = nuc980_dma_probe,
	.driver = {
		.name = "nuc980-pdma",
		.of_match_table = nuc980_dma_dt_ids,
	},
};

static int __init nuc980_dma_init(void)
{
	return platform_driver_register(&nuc980_dma_driver);
}
subsys_initcall(nuc980_dma_init);
