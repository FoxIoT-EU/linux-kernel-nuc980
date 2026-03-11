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
#include <linux/dma-mapping.h>
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
#include <sound/core.h>
#include <sound/dmaengine_pcm.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>

#define REG_GLBCON                     0x00
#define   GLBCON_I2S_EN                BIT(0)
#define   GLBCON_BITS_SELECT_16        (0x01 << 8)
#define   GLBCON_P_DMA_IRQ             BIT(10)
#define   GLBCON_R_DMA_IRQ             BIT(11)
#define   GLBCON_P_DMA_IRQ_SEL_END     (0x00 << 12)
#define   GLBCON_P_DMA_IRQ_SEL_HALF    (0x01 << 12)
#define   GLBCON_P_DMA_IRQ_SEL_QUARTER (0x02 << 12)
#define   GLBCON_P_DMA_IRQ_SEL_EIGHTH  (0x03 << 12)
#define   GLBCON_R_DMA_IRQ_SEL_END     (0x00 << 14)
#define   GLBCON_R_DMA_IRQ_SEL_HALF    (0x01 << 14)
#define   GLBCON_R_DMA_IRQ_SEL_QUARTER (0x02 << 14)
#define   GLBCON_R_DMA_IRQ_SEL_EIGHTH  (0x03 << 14)
#define   GLBCON_P_DMA_IRQ_EN          BIT(20)
#define   GLBCON_R_DMA_IRQ_EN          BIT(21)
#define REG_RESET                      0x04
#define   RESET_PLAY                   BIT(5)
#define   RESET_RECORD                 BIT(6)
#define   RESET_PLAY_SINGLE_STEREO     (0x03 << 12)
#define   RESET_RECORD_SINGLE_STEREO   (0x03 << 14)
#define REG_RDESB                      0x08
#define REG_RDES_LENGTH                0x0c
#define REG_RDESC                      0x10
#define REG_PDESB                      0x14
#define REG_PDES_LENGTH                0x18
#define REG_PDESC                      0x1c
#define REG_RSR                        0x20
#define   RSR_R_DMA_RIA_IRQ            BIT(0)
#define REG_PSR                        0x24
#define   PSR_P_DMA_RIA_IRQ            BIT(0)
#define REG_CON                        0x28
#define   CON_FORMAT                   BIT(3)
#define   CON_MCLK_SEL                 BIT(4)
#define   CON_SLAVE                    BIT(20)
#define NUC980_I2S_REG_SIZE            0x50

struct nuc980_i2s {
	void __iomem             *base;
	struct clk               *hclk;
	struct clk               *eclk;
	spinlock_t               lock;
	struct snd_pcm_substream *substream[2];
	dma_addr_t               dma_addr[2];
	unsigned long            buffersize[2];
	struct device            *dev;
};

static irqreturn_t nuc980_i2s_irq(int irq, void *dev_id)
{
	struct nuc980_i2s *ni2s = dev_id;
	unsigned long flags;
	int stream;
	u32 con;
	u32 rsr;
	u32 psr;

	spin_lock_irqsave(&ni2s->lock, flags);

	con = readl(ni2s->base + REG_GLBCON);

	if (con & GLBCON_R_DMA_IRQ) {
		stream = SNDRV_PCM_STREAM_CAPTURE;
		writel(con | GLBCON_R_DMA_IRQ, ni2s->base + REG_GLBCON);
		rsr = readl(ni2s->base + REG_RSR);

		if (rsr & RSR_R_DMA_RIA_IRQ) {
			rsr = RSR_R_DMA_RIA_IRQ;
			writel(rsr, ni2s->base + REG_RSR);
		}
	} else if (con & GLBCON_P_DMA_IRQ) {
		stream = SNDRV_PCM_STREAM_PLAYBACK;
		writel(con | GLBCON_P_DMA_IRQ, ni2s->base + REG_GLBCON);
		psr = readl(ni2s->base + REG_PSR);

		if (psr & PSR_P_DMA_RIA_IRQ) {
			psr = PSR_P_DMA_RIA_IRQ;
			writel(psr, ni2s->base + REG_PSR);
		}
	} else {
		spin_unlock_irqrestore(&ni2s->lock, flags);
		return IRQ_HANDLED;
	}

	spin_unlock_irqrestore(&ni2s->lock, flags);
	snd_pcm_period_elapsed(ni2s->substream[stream]);

	return IRQ_HANDLED;
}

static int nuc980_i2s_trigger(struct snd_pcm_substream *substream, int cmd,
					struct snd_soc_dai *dai)
{
	struct nuc980_i2s *ni2s = snd_soc_dai_get_drvdata(dai);
	u32 glbcon;
	u32 reset;

	glbcon = readl(ni2s->base + REG_GLBCON);
	reset = readl(ni2s->base + REG_RESET);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		glbcon |= GLBCON_I2S_EN;
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			glbcon |= GLBCON_P_DMA_IRQ_EN;
			writel(PSR_P_DMA_RIA_IRQ, ni2s->base + REG_PSR);
			reset |= RESET_PLAY;
		} else {
			glbcon |= GLBCON_R_DMA_IRQ_EN;
			writel(RSR_R_DMA_RIA_IRQ, ni2s->base + REG_RSR);
			reset |= RESET_RECORD;
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		glbcon &= ~GLBCON_I2S_EN;
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			glbcon &= ~GLBCON_P_DMA_IRQ_EN;
			writel(0x00, ni2s->base + REG_PSR);
			reset &= ~RESET_PLAY;
		} else {
			glbcon &= ~GLBCON_R_DMA_IRQ_EN;
			writel(0x00, ni2s->base + REG_RSR);
			reset &= ~RESET_RECORD;
		}
		break;
	default:
		return -EINVAL;
	}

	writel(reset, ni2s->base + REG_RESET);
	writel(glbcon, ni2s->base + REG_GLBCON);

	return 0;
}

static void nuc980_i2s_calc_con(unsigned long clk, unsigned long bclk, u32 *con)
{
	unsigned long diff_calc;
	unsigned long bclk_calc;
	unsigned long diff = -1;
	int bclk_div_final;
	int prs_final;
	int bclk_div;
	int prs;

	for (prs = 0; prs < 16; prs++) {
	//	if ((prs == 8) || (prs == 10) || (prs == 12) || (prs == 14))
	//		continue;

		for (bclk_div = 0; bclk_div < 8; bclk_div++) {
			bclk_calc = clk / ((1 + prs) * (bclk_div + 1) * 2);
			if (bclk_calc > bclk) {
				diff_calc = bclk_calc - bclk;
			} else {
				diff_calc = bclk - bclk_calc;
			}
			if (diff_calc < diff) {
				diff = diff_calc;
				prs_final = prs;
				bclk_div_final = bclk_div;
			}
		}
	}

	*con |= ((u32)prs_final << 16) | ((u32)bclk_div_final << 5);
}

static int nuc980_i2s_hw_params(struct snd_pcm_substream *substream,
					 struct snd_pcm_hw_params *params,
					 struct snd_soc_dai *dai)
{
	struct nuc980_i2s *ni2s = snd_soc_dai_get_drvdata(dai);
	unsigned long bclk;
	u32 glbcon;
	u32 con;

	if (params_width(params) != 16)
		return -EINVAL;

	if (params_channels(params) != 2)
		return -EINVAL;

	glbcon = readl(ni2s->base + REG_GLBCON);
	glbcon = (glbcon & ~0x300) | GLBCON_BITS_SELECT_16;
	writel(glbcon, ni2s->base + REG_GLBCON);

	con = readl(ni2s->base + REG_CON);
	con &= ~0x000f00e0;
	bclk = params_rate(params) * 16 * 2;
	nuc980_i2s_calc_con(clk_get_rate(ni2s->eclk), bclk, &con);
	writel(con, ni2s->base + REG_CON);

	return 0;
}

static int nuc980_i2s_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct nuc980_i2s *ni2s = snd_soc_dai_get_drvdata(dai);
	u32 con;

	con = readl(ni2s->base + REG_CON);

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_MSB:
		con |= CON_FORMAT;
		break;
	case SND_SOC_DAIFMT_I2S:
		con &= ~CON_FORMAT;
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		con |= CON_SLAVE;
		break;
	case SND_SOC_DAIFMT_CBS_CFS:
		con &= ~CON_SLAVE;
		break;
	default:
		return -EINVAL;
	}
	writel(con, ni2s->base + REG_CON);
	return 0;
}

static const struct snd_pcm_hardware nuc980_i2s_pcm_hardware = {
	.info             = SNDRV_PCM_INFO_INTERLEAVED |
				SNDRV_PCM_INFO_BLOCK_TRANSFER |
				SNDRV_PCM_INFO_MMAP |
				SNDRV_PCM_INFO_MMAP_VALID |
				SNDRV_PCM_INFO_PAUSE |
				SNDRV_PCM_INFO_RESUME,
	.formats          = SNDRV_PCM_FMTBIT_S16_LE,
	.channels_min     = 2,
	.channels_max     = 2,
	.period_bytes_min = 1 * 1024,
	.period_bytes_max = 4 * 1024,
	.periods_min      = 1,
	.periods_max      = 1024,
	.buffer_bytes_max = 4 * 1024,
};

static u64 nuc980_i2s_pcm_dmamask = DMA_BIT_MASK(32);
static int nuc980_i2s_pcm_new(struct snd_soc_pcm_runtime *rtd,
					struct snd_soc_dai *dai)
{
	struct snd_card *card = rtd->card->snd_card;
	struct snd_pcm *pcm = rtd->pcm;

	if (!card->dev->dma_mask)
		card->dev->dma_mask = &nuc980_i2s_pcm_dmamask;
	if (!card->dev->coherent_dma_mask)
		card->dev->coherent_dma_mask = DMA_BIT_MASK(32);

	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV,
		card->dev, 4 * 1024, (4 * 1024) - 1);

	return 0;
}

static int nuc980_i2s_pcm_open(struct snd_soc_component *component,
					struct snd_pcm_substream *substream)
{
	snd_soc_set_runtime_hwparams(substream, &nuc980_i2s_pcm_hardware);
	return 0;
}

static int nuc980_i2s_pcm_close(struct snd_soc_component *component,
					struct snd_pcm_substream *substream)
{
	return 0;
}

static int nuc980_i2s_pcm_hw_params(struct snd_soc_component *component,
					struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct nuc980_i2s *ni2s = snd_soc_component_get_drvdata(component);
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&ni2s->lock, flags);

	if(runtime->dma_addr == 0) {
		ret = snd_pcm_lib_malloc_pages(substream,
						params_buffer_bytes(params));

		if (ret < 0)
			goto out;

		ni2s->substream[substream->stream] = substream;
	}

	ret = 0;
	ni2s->dma_addr[substream->stream] = runtime->dma_addr | 0x80000000;
	ni2s->buffersize[substream->stream] = params_buffer_bytes(params);

out:
	spin_unlock_irqrestore(&ni2s->lock, flags);
	return ret;
}

static snd_pcm_uframes_t nuc980_i2s_pcm_pointer(
					struct snd_soc_component *component,
					struct snd_pcm_substream *substream)
{
	struct nuc980_i2s *ni2s = snd_soc_component_get_drvdata(component);
	struct snd_pcm_runtime *runtime = substream->runtime;
	snd_pcm_uframes_t frames;

	spin_lock(&ni2s->lock);

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		frames = bytes_to_frames(substream->runtime,
			readl(ni2s->base + REG_RDESC) - runtime->dma_addr);
	else
		frames = bytes_to_frames(substream->runtime,
			readl(ni2s->base + REG_PDESC) - runtime->dma_addr);

	spin_unlock(&ni2s->lock);

	return frames;
}

static int nuc980_i2s_pcm_prepare(struct snd_soc_component *component,
					struct snd_pcm_substream *substream)
{
	struct nuc980_i2s *ni2s = snd_soc_component_get_drvdata(component);
	unsigned long flags;
	u32 glbcon;
	u32 reset;
	int ret = 0;

	spin_lock_irqsave(&ni2s->lock, flags);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		writel(ni2s->dma_addr[substream->stream],
						ni2s->base + REG_PDESB);
		writel(ni2s->buffersize[substream->stream],
						ni2s->base + REG_PDES_LENGTH);
	} else {
		writel(ni2s->dma_addr[substream->stream],
						ni2s->base + REG_RDESB);
		writel(ni2s->buffersize[substream->stream],
						ni2s->base + REG_RDES_LENGTH);
	}

	reset = readl(ni2s->base + REG_RESET);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		reset |= RESET_PLAY_SINGLE_STEREO;
	else
		reset |= RESET_RECORD_SINGLE_STEREO;

	writel(reset, ni2s->base + REG_RESET);

	/* set DMA IRQ to half */
	glbcon = readl(ni2s->base + REG_GLBCON);
	glbcon &= ~0xf000;
	glbcon |= (GLBCON_R_DMA_IRQ_SEL_HALF | GLBCON_P_DMA_IRQ_SEL_HALF);
	writel(glbcon, ni2s->base + REG_GLBCON);

	spin_unlock_irqrestore(&ni2s->lock, flags);
	return ret;
}

static int nuc980_i2s_pcm_hw_free(struct snd_soc_component *component,
					struct snd_pcm_substream *substream)
{
	struct nuc980_i2s *ni2s = snd_soc_component_get_drvdata(component);

	snd_pcm_lib_free_pages(substream);
	ni2s->substream[substream->stream] = NULL;
	return 0;
}


static const struct snd_soc_dai_ops nuc980_i2s_dai_ops = {
	.trigger    = nuc980_i2s_trigger,
	.hw_params  = nuc980_i2s_hw_params,
	.set_fmt    = nuc980_i2s_set_fmt,
	.pcm_new    = nuc980_i2s_pcm_new,
};

static struct snd_soc_dai_driver nuc980_i2s_dai_driver = {
	.playback = {
		.rates        = SNDRV_PCM_RATE_8000_48000,
		.formats      = SNDRV_PCM_FMTBIT_S16_LE,
		.channels_min = 2,
		.channels_max = 2,
	},
	.capture  = {
		.rates        = SNDRV_PCM_RATE_8000_48000,
		.formats      = SNDRV_PCM_FMTBIT_S16_LE,
		.channels_min = 2,
		.channels_max = 2,
	},
	.ops      = &nuc980_i2s_dai_ops,
};

static const struct snd_soc_component_driver nuc980_i2s_component_driver = {
	.name      = "nuc980-i2s",
	.open      = nuc980_i2s_pcm_open,
	.close     = nuc980_i2s_pcm_close,
	.hw_params = nuc980_i2s_pcm_hw_params,
	.pointer   = nuc980_i2s_pcm_pointer,
	.prepare   = nuc980_i2s_pcm_prepare,
	.hw_free   = nuc980_i2s_pcm_hw_free,
};

static int nuc980_i2s_probe(struct platform_device *pdev)
{
	struct nuc980_i2s *ni2s;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct reset_control *rst;
	struct resource res;
	int irq;
	int ret;

	ni2s = devm_kzalloc(dev, sizeof(*ni2s), GFP_KERNEL);
	if (!ni2s) {
		dev_err(dev, "unable to allocate memory\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, ni2s);
	spin_lock_init(&ni2s->lock);
	ni2s->dev = dev;

	ret = of_address_to_resource(np, 0, &res);
	if (ret) {
		dev_err(dev, "unable to get mem region\n");
		return -EINVAL;
	}

	if (!res.start || resource_size(&res) < NUC980_I2S_REG_SIZE) {
		dev_err(dev, "mem region out of range\n");
		return -EINVAL;
	}

	dev_set_name(dev, "%08x.nuc980-i2s", (unsigned int)res.start);

	ni2s->eclk = devm_clk_get(dev, "eclk");
	if (IS_ERR(ni2s->eclk)) {
		dev_err(dev, "unable to get clock: eclk\n");
		return PTR_ERR(ni2s->eclk);
	}

	ni2s->hclk = devm_clk_get(dev, "hclk");
	if (IS_ERR(ni2s->hclk)) {
		dev_warn(dev, "unable to get clock: hclk\n");
		ni2s->hclk = NULL;
	}

	ret = clk_prepare_enable(ni2s->eclk);
	if (ret) {
		dev_err(dev, "unable to enable clock: eclk\n");
		return ret;
	}

	ret = clk_prepare_enable(ni2s->hclk);
	if (ret) {
		dev_err(dev, "unable to enable clock: hclk\n");
		goto disable_eclk;
	}

	rst = devm_reset_control_get_optional_shared(dev, NULL);
	if (!IS_ERR(rst))
		reset_control_deassert(rst);

	ni2s->base = devm_ioremap_resource(dev, &res);
	if (IS_ERR(ni2s->base)) {
		dev_err(dev, "unable to map mem region\n");
		ret = PTR_ERR(ni2s->base);
		goto disable_hclk;
	}

	irq = irq_of_parse_and_map(np, 0);
	if (irq <= 0) {
		dev_err(dev, "unable to get irq\n");
		ret = -EINVAL;
		goto disable_hclk;
	}

	ret = devm_request_irq(dev, irq, nuc980_i2s_irq,
					0, dev_name(dev), ni2s);

	if (ret) {
		dev_err(dev, "unable to request irq\n");
		goto disable_hclk;
	}

	ret = devm_snd_soc_register_component(dev, &nuc980_i2s_component_driver,
						&nuc980_i2s_dai_driver, 1);

	if (ret) {
		dev_err(dev, "unable to register component\n");
		goto disable_hclk;
	}

	dev_info(dev, "initialized\n");

	return 0;

disable_hclk:
	clk_disable_unprepare(ni2s->hclk);
disable_eclk:
	clk_disable_unprepare(ni2s->eclk);
	return ret;
}

static const struct of_device_id nuc980_i2s_dt_ids[] = {
	{ .compatible = "nuvoton,nuc980-i2s" },
	{},
};
MODULE_DEVICE_TABLE(of, nuc980_i2s_dt_ids);

static struct platform_driver nuc980_i2s_driver = {
	.probe = nuc980_i2s_probe,
	.driver = {
		.name = "nuc980-i2s",
		.of_match_table = nuc980_i2s_dt_ids,
	},
};

static int __init nuc980_i2s_init(void)
{
	return platform_driver_register(&nuc980_i2s_driver);
}
device_initcall(nuc980_i2s_init);
