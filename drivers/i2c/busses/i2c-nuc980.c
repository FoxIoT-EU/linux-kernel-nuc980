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
#include <linux/i2c.h>
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

#define REG_CTL0            0x000
#define   CTL0_AA           BIT(2)
#define   CTL0_SI           BIT(3)
#define   CTL0_STO          BIT(4)
#define   CTL0_STA          BIT(5)
#define   CTL0_EN           BIT(6)
#define   CTL0_IEN          BIT(7)
#define REG_ADDR0           0x004
#define REG_DAT             0x008
#define REG_STATUS0         0x00c
#define   STATUS0_BUS_ERR   0x00
#define   STATUS0_STA       0x08
#define   STATUS0_REP_STA   0x10
#define   STATUS0_SADD_ACK  0x18
#define   STATUS0_SADD_NACK 0x20
#define   STATUS0_SDAT_ACK  0x28
#define   STATUS0_SDAT_NACK 0x30
#define   STATUS0_ARB_LOST  0x38
#define   STATUS0_RADD_ACK  0x40
#define   STATUS0_RADD_NACK 0x48
#define   STATUS0_RDAT_ACK  0x50
#define   STATUS0_RDAT_NACK 0x58
#define REG_CLKDIV          0x010
#define REG_TOCTL           0x014
#define REG_ADDR1           0x018
#define REG_ADDR2           0x01c
#define REG_ADDR3           0x020
#define REG_ADDRMSK0        0x024
#define REG_ADDRMSK1        0x028
#define REG_ADDRMSK2        0x02c
#define REG_ADDRMSK3        0x030
#define REG_WKCTL           0x03c
#define REG_WKSTS           0x040
#define REG_CTL1            0x044
#define REG_STATUS1         0x048
#define REG_TMCTL           0x04c
#define REG_BUSCTL          0x050
#define REG_BUSTCTL         0x054
#define REG_BUSSTS          0x058
#define REG_PKTSIZE         0x05c
#define REG_PKTCRC          0x060
#define REG_BUSTOUT         0x064
#define REG_CLKTOUT         0x068
#define NUC980_I2C_REG_SIZE 0x100

struct nuc980_i2c {
	void __iomem         *base;
	struct clk           *clk;
	struct reset_control *rst;
	struct i2c_adapter   adap;
	struct device        *dev;
	struct completion    done;
	struct i2c_msg       *msgs;
	int                  msg_idx;
	int                  msg_num;
	int                  msg_ptr;
	int                  msg_state;
	bool                 needs_reset;
};

enum {
	MSG_STATE_WAIT_START,
	MSG_STATE_WAIT_RADD,
	MSG_STATE_WAIT_SADD,
	MSG_STATE_WAIT_RDAT,
	MSG_STATE_WAIT_SDAT,
};

static void nuc980_i2c_start(struct nuc980_i2c *ni2c)
{
	writel((readl(ni2c->base + REG_CTL0) & ~0x3c) | CTL0_STA | CTL0_SI,
						ni2c->base + REG_CTL0);
}

static void nuc980_i2c_recv(struct nuc980_i2c *ni2c)
{
	writel((readl(ni2c->base + REG_CTL0) & ~0x3c) | CTL0_AA | CTL0_SI,
						ni2c->base + REG_CTL0);
}

static void nuc980_i2c_send(struct nuc980_i2c *ni2c, u8 data)
{
	writel(data, ni2c->base + REG_DAT);
	writel((readl(ni2c->base + REG_CTL0) & ~0x3c) | CTL0_SI,
						ni2c->base + REG_CTL0);
}

static void nuc980_i2c_done(struct nuc980_i2c *ni2c)
{
	writel(readl(ni2c->base + REG_CTL0) & ~CTL0_IEN,
						ni2c->base + REG_CTL0);
	complete(&ni2c->done);
}

static void nuc980_i2c_step(struct nuc980_i2c *ni2c, u32 status0)
{
	struct i2c_msg *msg = &ni2c->msgs[ni2c->msg_idx];

	if (ni2c->msg_state == MSG_STATE_WAIT_START) {
		if ((status0 == STATUS0_STA) || (status0 == STATUS0_REP_STA)) {
			if (msg->flags & I2C_M_RD)
				ni2c->msg_state = MSG_STATE_WAIT_RADD;
			else
				ni2c->msg_state = MSG_STATE_WAIT_SADD;

			nuc980_i2c_send(ni2c, i2c_8bit_addr_from_msg(msg));
		} else {
			nuc980_i2c_done(ni2c);
		}
	} else if (ni2c->msg_state == MSG_STATE_WAIT_RADD) {
		if (status0 == STATUS0_RADD_ACK) {
			if (!msg->len) {
				ni2c->msg_idx++;
				if (ni2c->msg_idx >= ni2c->msg_num) {
					nuc980_i2c_done(ni2c);
				} else {
					ni2c->msg_state = MSG_STATE_WAIT_START;
					nuc980_i2c_start(ni2c);
				}
			} else {
				ni2c->msg_state = MSG_STATE_WAIT_RDAT;
				nuc980_i2c_recv(ni2c);
			}
		} else {
			ni2c->needs_reset = true;
			nuc980_i2c_done(ni2c);
		}
	} else if (ni2c->msg_state == MSG_STATE_WAIT_SADD) {
		if (status0 == STATUS0_SADD_ACK) {
			if (!msg->len) {
				ni2c->msg_idx++;
				if (ni2c->msg_idx >= ni2c->msg_num) {
					nuc980_i2c_done(ni2c);
				} else {
					ni2c->msg_state = MSG_STATE_WAIT_START;
					nuc980_i2c_start(ni2c);
				}
			} else {
				ni2c->msg_state = MSG_STATE_WAIT_SDAT;
				nuc980_i2c_send(ni2c, msg->buf[ni2c->msg_ptr]);
				ni2c->msg_ptr++;
			}
		} else {
			ni2c->needs_reset = true;
			nuc980_i2c_done(ni2c);
		}
	} else if (ni2c->msg_state == MSG_STATE_WAIT_RDAT) {
		if (status0 == STATUS0_RDAT_ACK) {
			msg->buf[ni2c->msg_ptr] = readb(ni2c->base + REG_DAT);
			ni2c->msg_ptr++;
			if (ni2c->msg_ptr >= msg->len) {
				ni2c->msg_ptr = 0;
				ni2c->msg_idx++;
				if (ni2c->msg_idx >= ni2c->msg_num) {
					nuc980_i2c_done(ni2c);
				} else {
					ni2c->msg_state = MSG_STATE_WAIT_START;
					nuc980_i2c_start(ni2c);
				}
			} else {
				nuc980_i2c_recv(ni2c);
			}
		} else {
			nuc980_i2c_done(ni2c);
		}
	} else if (ni2c->msg_state == MSG_STATE_WAIT_SDAT) {
		if (status0 == STATUS0_SDAT_ACK) {
			if (ni2c->msg_ptr >= msg->len) {
				ni2c->msg_ptr = 0;
				ni2c->msg_idx++;
				if (ni2c->msg_idx >= ni2c->msg_num) {
					nuc980_i2c_done(ni2c);
				} else {
					ni2c->msg_state = MSG_STATE_WAIT_START;
					nuc980_i2c_start(ni2c);
				}
			} else {
				nuc980_i2c_send(ni2c, msg->buf[ni2c->msg_ptr]);
				ni2c->msg_ptr++;
			}
		} else {
			nuc980_i2c_done(ni2c);
		}
	}
}

static irqreturn_t nuc980_i2c_irq(int irq, void *dev_id)
{
	struct nuc980_i2c *ni2c = dev_id;
	u32 status0;

	status0 = readl(ni2c->base + REG_STATUS0);

	if (status0 == STATUS0_ARB_LOST) {
		dev_err(ni2c->dev, "arbitration lost\n");
		ni2c->needs_reset = true;
		nuc980_i2c_done(ni2c);
		return IRQ_HANDLED;
	}

	if (status0 == STATUS0_BUS_ERR) {
		dev_err(ni2c->dev, "bus error\n");
		ni2c->needs_reset = true;
		nuc980_i2c_done(ni2c);
		return IRQ_HANDLED;
	}

	nuc980_i2c_step(ni2c, status0);

	return IRQ_HANDLED;
}

static void nuc980_i2c_reset(struct nuc980_i2c *ni2c)
{
	u32 clkdiv;

	clkdiv = readl(ni2c->base + REG_CLKDIV);
	reset_control_reset(ni2c->rst);
	udelay(10);
	writel(clkdiv, ni2c->base + REG_CLKDIV);
	writel(CTL0_EN, ni2c->base + REG_CTL0);
}

static int nuc980_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
								int num)
{
	struct nuc980_i2c *ni2c = i2c_get_adapdata(adap);
	u32 val;
	int ret;

	ni2c->msgs      = msgs;
	ni2c->msg_idx   = 0;
	ni2c->msg_num   = num;
	ni2c->msg_ptr   = 0;
	ni2c->msg_state = MSG_STATE_WAIT_START;

	reinit_completion(&ni2c->done);
	writel(readl(ni2c->base + REG_CTL0) | CTL0_IEN,
						ni2c->base + REG_CTL0);

	nuc980_i2c_start(ni2c);

	ret = wait_for_completion_interruptible_timeout(&ni2c->done,
						msecs_to_jiffies(200));

	writel(readl(ni2c->base + REG_CTL0) & ~CTL0_IEN,
						ni2c->base + REG_CTL0);

	if (ret <= 0)
		ni2c->needs_reset = true;

	if (readl(ni2c->base + REG_CTL0) & CTL0_AA) {
		writel(readl(ni2c->base + REG_CTL0) & ~CTL0_AA,
						ni2c->base + REG_CTL0);
		mdelay(1);
	}

	writel((readl(ni2c->base + REG_CTL0) & ~0x3c) | CTL0_STO | CTL0_SI,
						ni2c->base + REG_CTL0);

	if (ni2c->needs_reset) {
		nuc980_i2c_reset(ni2c);
		ni2c->needs_reset = false;
	} else {
		if (readl_poll_timeout(ni2c->base + REG_CTL0, val,
						!(val & CTL0_STO), 10, 1000))
			nuc980_i2c_reset(ni2c);
	}

	if (ret < 0)
		return ret;

	if (ret == 0)
		return -ETIMEDOUT;

	return ni2c->msg_idx;
}

static u32 nuc980_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_PROTOCOL_MANGLING | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm nuc980_i2c_algo = {
	.master_xfer   = nuc980_i2c_xfer,
	.functionality = nuc980_i2c_func,
};

static int nuc980_i2c_probe(struct platform_device *pdev)
{
	struct nuc980_i2c *ni2c;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct i2c_adapter *adap;
	struct resource res;
	u32 clk_frequency;
	int irq;
	int ret;

	ni2c = devm_kzalloc(dev, sizeof(*ni2c), GFP_KERNEL);
        if (!ni2c) {
                dev_err(dev, "unable to allocate memory\n");
                return -ENOMEM;
        }

	platform_set_drvdata(pdev, ni2c);
	ni2c->dev = dev;
	init_completion(&ni2c->done);
	adap = &ni2c->adap;

	ret = of_address_to_resource(np, 0, &res);
	if (ret) {
		dev_err(dev, "unable to get mem region\n");
		return ret;
	}

	if (!res.start || resource_size(&res) < NUC980_I2C_REG_SIZE) {
		dev_err(dev, "mem region out of range\n");
		return -EINVAL;
	}

	ni2c->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(ni2c->clk)) {
		dev_err(dev, "unable to get clock\n");
		return PTR_ERR(ni2c->clk);
	}

	ret = clk_prepare_enable(ni2c->clk);
	if (ret) {
		dev_err(dev, "unable to enable clock\n");
		return ret;
	}

	ni2c->rst = devm_reset_control_get(dev, NULL);
	if (IS_ERR(ni2c->rst))
		ni2c->rst = NULL;

	reset_control_deassert(ni2c->rst);

	ni2c->base = devm_ioremap_resource(dev, &res);
	if (IS_ERR(ni2c->base)) {
		dev_err(dev, "unable to map mem region\n");
                ret = PTR_ERR(ni2c->base);
		goto disable_clk;
	}

	if (of_property_read_u32(np, "clock-frequency", &clk_frequency))
		clk_frequency = 100000;

	clk_frequency = (clk_get_rate(ni2c->clk) / (clk_frequency * 4) - 1);
	writel(clk_frequency & 0xffff, ni2c->base + REG_CLKDIV);
	writel(CTL0_EN, ni2c->base + REG_CTL0);

	irq = irq_of_parse_and_map(np, 0);
	if (irq <= 0) {
		dev_err(dev, "unable to get irq\n");
		ret = -EINVAL;
		goto disable_clk;
	}

	ret = devm_request_irq(dev, irq, nuc980_i2c_irq,
					0, dev_name(dev), ni2c);

	if (ret) {
		dev_err(dev, "unable to request irq\n");
		goto disable_clk;
	}

	i2c_set_adapdata(adap, ni2c);
	strscpy(adap->name, dev_name(dev), sizeof(adap->name));
	adap->owner       = THIS_MODULE;
	adap->timeout     = 2 * HZ;
	adap->retries     = 0;
	adap->algo        = &nuc980_i2c_algo;
	adap->dev.parent  = dev;
	adap->dev.of_node = np;
	adap->class       = I2C_CLASS_HWMON;

	ret = i2c_add_adapter(adap);
	if (ret) {
		dev_err(dev, "unable to add i2c adapter\n");
		goto disable_clk;
	}

	dev_info(dev, "initialized\n");

	return 0;

disable_clk:
	clk_disable_unprepare(ni2c->clk);
	return ret;
}

static const struct of_device_id nuc980_i2c_dt_ids[] = {
	{ .compatible = "nuvoton,nuc980-i2c" },
	{},
};
MODULE_DEVICE_TABLE(of, nuc980_i2c_dt_ids);

static struct platform_driver nuc980_i2c_driver = {
	.probe = nuc980_i2c_probe,
	.driver = {
		.name = "nuc980-i2c",
		.of_match_table = nuc980_i2c_dt_ids,
	},
};

static int __init nuc980_i2c_init(void)
{
        return platform_driver_register(&nuc980_i2c_driver);
}
device_initcall(nuc980_i2c_init);
