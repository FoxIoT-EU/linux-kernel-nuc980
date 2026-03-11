// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020, Veiko Rütter <rebane@alkohol.ee>
 *
 * Based on previous work from:
 * Copyright (C) 2018 Nuvoton Technology Corp.
 *
 */

#include <linux/clk.h>
#include <linux/ctype.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/gfp.h>
#include <linux/if_link.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/mii.h>
#include <linux/module.h>
#include <linux/net_tstamp.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>

#define REG_CAMCMR           0x000
#define   CAMCMR_AUP         BIT(0)
#define   CAMCMR_AMP         BIT(1)
#define   CAMCMR_ABP         BIT(2)
#define   CAMCMR_ECMP        BIT(4)
#define REG_CAMEN            0x004
#define   CAMEN_EN0          BIT(0)
#define REG_CAMM_BASE        0x008
#define REG_CAML_BASE        0x00c
#define   CAM_ENTRY_SIZE     BIT(3)
#define REG_TXDLSA           0x088
#define REG_RXDLSA           0x08c
#define REG_MCMDR            0x090
#define   MCMDR_RXON         BIT(0)
#define   MCMDR_ACP          BIT(3)
#define   MCMDR_SPCRC        BIT(5)
#define   MCMDR_MGPWAKE      BIT(6)
#define   MCMDR_TXON         BIT(8)
#define   MCMDR_FDUP         BIT(18)
#define   MCMDR_ENMDC        BIT(19)
#define   MCMDR_OPMOD        BIT(20)
#define   MCMDR_SWR          BIT(24)
#define REG_MIID             0x094
#define REG_MIIDA            0x098
#define   MIIDA_PHYAD        BIT(8)
#define   MIIDA_PHYWR        BIT(16)
#define   MIIDA_PHYBUSY      BIT(17)
#define   MIIDA_PHYPRESP     BIT(18)
#define   MIIDA_MDCON        BIT(19)
#define REG_FFTCR            0x09c
#define REG_TSDR             0x0a0
#define   TSDR_START         BIT(0)
#define REG_RSDR             0x0a4
#define   RSDR_START         BIT(0)
#define REG_DMARFC           0x0a8
#define REG_MIEN             0x0ac
#define   MIEN_RXINTR        BIT(0)
#define   MIEN_RXGD          BIT(4)
#define   MIEN_RDU           BIT(10)
#define   MIEN_RXBERR        BIT(11)
#define   MIEN_WOL           BIT(15)
#define   MIEN_TXINTR        BIT(16)
#define   MIEN_TXCP          BIT(18)
#define   MIEN_TXABT         BIT(21)
#define   MIEN_TXBERR        BIT(24)
#define REG_MISTA            0x0b0
#define   MISTA_RXOV         BIT(2)
#define   MISTA_RXGD         BIT(4)
#define   MISTA_RDU          BIT(10)
#define   MISTA_RXBERR       BIT(11)
#define   MISTA_WOL          BIT(15)
#define   MISTA_TXEMP        BIT(17)
#define   MISTA_EXDEF        BIT(19)
#define   MISTA_TDU          BIT(23)
#define   MISTA_TXBERR       BIT(24)
#define REG_CTXDSA           0x0cc
#define REG_CTXBSA           0x0d0
#define REG_CRXDSA           0x0d4
#define REG_CRXBSA           0x0d8
#define NUC980_EMAC_REG_SIZE 0x100

#define   TXBD_MODE_PADDING  BIT(0)
#define   TXBD_MODE_CRC      BIT(1)
#define   TXBD_MODE_TXINTEN  BIT(2)
#define   TXBD_MODE_OWN_DMA  BIT(31)

#define   TXBD_SL_TXCP       BIT(19)

#define   RXBD_SL_CRCE       BIT(17)
#define   RXBD_SL_PTLE       BIT(19)
#define   RXBD_SL_RXGD       BIT(20)
#define   RXBD_SL_ALIE       BIT(21)
#define   RXBD_SL_RP         BIT(22)
#define   RXBD_SL_OWN_DMA    BIT(31)

#define MAX_PACKET_SIZE      1522
#define TX_QUEUE_LEN	     32
#define RX_QUEUE_LEN	     128
#define MII_TIMEOUT          10000

struct nuc980_emac_txbd {
	__le32 mode;
	__le32 buffer;
	__le32 sl;
	__le32 next;
};

struct nuc980_emac_rxbd {
	__le32 sl;
	__le32 buffer;
	__le32 reserved;
	__le32 next;
};

struct nuc980_emac {
	struct device            *dev;
	struct net_device        *ndev;
	struct mii_bus           *mii_bus;
	struct device_node       *phy_node;
	phy_interface_t          phy_interface;
	struct phy_device        *phydev;
	struct resource          res;
	void __iomem             *base;
	struct clk               *hclk;
	struct clk               *mclk;
	struct clk               *eclk;
//	struct clk               *rclk; // 50MHz rmii clk?
	struct reset_control     *rst;
	struct reset_control     *mrst;
	int                      txirq;
	int                      rxirq;
	spinlock_t               lock;
	int                      link;
	int                      speed;
	int                      duplex;
	struct sk_buff           *tx_skb[TX_QUEUE_LEN];
	struct sk_buff           *rx_skb[RX_QUEUE_LEN];
	struct nuc980_emac_txbd  *txbd;
	struct nuc980_emac_rxbd  *rxbd;
	dma_addr_t               txbd_phys;
	dma_addr_t               rxbd_phys;
	struct rtnl_link_stats64 stats;
	struct napi_struct       napi;
	unsigned                 cur_tx;
	unsigned                 cur_rx;
	unsigned                 finish_tx;
	bool                     needs_reset;
};

static void nuc980_nemac_write_cam(struct nuc980_emac *nemac, unsigned x,
								const u8 *p)
{
	u32 msw;
	u32 lsw;

	msw = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
	lsw = (p[4] << 24) | (p[5] << 16);

	writel(lsw, nemac->base + REG_CAML_BASE + x * CAM_ENTRY_SIZE);
	writel(msw, nemac->base + REG_CAMM_BASE + x * CAM_ENTRY_SIZE);
}

static void nuc980_emac_free_buffers(struct nuc980_emac *nemac)
{
	struct device *dev = nemac->dev;
	int i;

	for (i = 0; i < TX_QUEUE_LEN; i++) {
		if(!nemac->tx_skb[i])
			continue;

		dma_unmap_single(dev, (dma_addr_t)nemac->txbd[i].buffer,
					nemac->tx_skb[i]->len, DMA_TO_DEVICE);
		dev_kfree_skb_any(nemac->tx_skb[i]);
		nemac->tx_skb[i] = NULL;
	}

	for (i = 0; i < RX_QUEUE_LEN; i++) {
		if(!nemac->rx_skb[i])
			continue;

		dma_unmap_single(dev, (dma_addr_t)nemac->rxbd[i].buffer,
					MAX_PACKET_SIZE, DMA_FROM_DEVICE);
		dev_kfree_skb_any(nemac->rx_skb[i]);
		nemac->rx_skb[i] = NULL;
	}

	if (nemac->txbd) {
		dma_free_coherent(dev,
				sizeof(struct nuc980_emac_txbd) * TX_QUEUE_LEN,
				nemac->txbd, nemac->txbd_phys);
		nemac->txbd = NULL;
	}

	if (nemac->rxbd) {
		dma_free_coherent(dev,
				sizeof(struct nuc980_emac_rxbd) * RX_QUEUE_LEN,
				nemac->rxbd, nemac->rxbd_phys);
		nemac->rxbd = NULL;
	}
}

static int nuc980_emac_alloc_buffers(struct nuc980_emac *nemac)
{
	struct net_device *ndev = nemac->ndev;
	struct device *dev = nemac->dev;
	struct nuc980_emac_txbd *txbd;
	struct nuc980_emac_rxbd *rxbd;
	struct sk_buff *skb;
	unsigned offset;
	int ret;
	int i;

	nemac->txbd = dma_alloc_coherent(dev,
					sizeof(*nemac->txbd) * TX_QUEUE_LEN,
					&nemac->txbd_phys, GFP_KERNEL);

	if (!nemac->txbd) {
		dev_err(dev, "unable to allocate tx desc\n");
		return -ENOMEM;
	}

	nemac->rxbd = dma_alloc_coherent(dev,
					sizeof(*nemac->txbd) * RX_QUEUE_LEN,
					&nemac->rxbd_phys, GFP_KERNEL);

	if (!nemac->rxbd) {
		dev_err(dev, "unable to allocate rx desc\n");
		ret = -ENOMEM;
		goto free_desc;
	}

	for (i = 0; i < TX_QUEUE_LEN; i++) {
		if (i == TX_QUEUE_LEN - 1)
			offset = 0;
		else
			offset = sizeof(struct nuc980_emac_txbd) * (i + 1);

		nemac->tx_skb[i] = NULL;

		txbd = &nemac->txbd[i];

		txbd->mode   = TXBD_MODE_PADDING | TXBD_MODE_CRC |
					TXBD_MODE_TXINTEN;
		txbd->buffer = 0;
		txbd->sl     = 0x00;
		txbd->next   = nemac->txbd_phys + offset;
	}

	for (i = 0; i < RX_QUEUE_LEN; i++) {
		if (i == RX_QUEUE_LEN - 1)
			offset = 0;
		else
			offset = sizeof(struct nuc980_emac_rxbd) * (i + 1);

		skb = netdev_alloc_skb(ndev, MAX_PACKET_SIZE + NET_IP_ALIGN);
		if (!skb) {
			dev_err(dev, "unable to allocate rx skb\n");
			ret = -ENOMEM;
			goto free_desc;
		}
		skb_reserve(skb, NET_IP_ALIGN);

		nemac->rx_skb[i] = skb;

		rxbd = &nemac->rxbd[i];

		rxbd->sl     = RXBD_SL_OWN_DMA;
		rxbd->buffer = dma_map_single(dev, skb->data,
					MAX_PACKET_SIZE, DMA_FROM_DEVICE);
		rxbd->next   = nemac->rxbd_phys + offset;
	}

	return 0;

free_desc:
	nuc980_emac_free_buffers(nemac);
	return ret;
}

static void nuc980_emac_get_and_clear_int(struct nuc980_emac *nemac,
						u32 *val, u32 mask)
{
	*val = readl(nemac->base + REG_MISTA) & mask;
	writel(*val, nemac->base + REG_MISTA);
}

static void nuc980_emac_default_idle(struct nuc980_emac *nemac)
{
	u32 saved_bits;
	u32 val;

	val = readl(nemac->base + REG_MCMDR);
	saved_bits = val & (MCMDR_FDUP | MCMDR_OPMOD);
	val |= MCMDR_SWR;
	writel(val, nemac->base + REG_MCMDR);

	/* During the emac reset the AHB will read 0 from all registers,
	 * so in order to see if the reset finished we can't count on
	 * (nemac->base + REG_MCMDR).SWR to become 0, instead we read another
	 * register that its reset value is not 0.
	 * We choose (nemac->base + REG_FFTCR).
	 */
	do {
		val = readl(nemac->base + REG_FFTCR);
	} while (val == 0);

	/* Now we can verify if (nemac->base + REG_MCMDR).SWR became 0.
	 * (probably it will be 0 on the first read).
	 */
	do {
		val = readl(nemac->base + REG_MCMDR);
	} while (val & MCMDR_SWR);

	/* restore values */
	writel(saved_bits, nemac->base + REG_MCMDR);
}

static int nuc980_emac_reset_mac(struct nuc980_emac *nemac)
{
	struct net_device *ndev = nemac->ndev;
	u32 val;
	int ret;

	/* disable tx and rx */
	writel(readl(nemac->base + REG_MCMDR) & ~(MCMDR_TXON | MCMDR_RXON),
						nemac->base + REG_MCMDR);

	nuc980_emac_default_idle(nemac);

	/* set fifo threshold */
	writel((0x03 << 8) | (0x01 << 20), nemac->base + REG_FFTCR);

	if (!netif_queue_stopped(ndev))
		netif_stop_queue(ndev);

	nuc980_emac_free_buffers(nemac);
	ret = nuc980_emac_alloc_buffers(nemac);
	if (ret)
		return ret;

	nemac->cur_tx = 0x0;
	nemac->finish_tx = 0x0;
	nemac->cur_rx = 0x0;

	/* set curdest */
	writel(nemac->txbd_phys, nemac->base + REG_TXDLSA);
	writel(nemac->rxbd_phys, nemac->base + REG_RXDLSA);

	/* enable cam */
	nuc980_nemac_write_cam(nemac, 0, ndev->dev_addr);
	writel(readl(nemac->base + REG_CAMEN) | CAMEN_EN0,
						nemac->base + REG_CAMEN);

	/* enable cam command */
	writel(CAMCMR_ECMP | CAMCMR_ABP | CAMCMR_AMP, nemac->base + REG_CAMCMR);

	/* enable mac interrupt */
	val = MIEN_TXINTR | MIEN_RXINTR | MIEN_RXGD | MIEN_TXCP | MIEN_RDU;
	val |= MIEN_TXBERR | MIEN_RXBERR | MIEN_TXABT | MIEN_WOL;
	writel(val, nemac->base + REG_MIEN);

	if (netif_queue_stopped(ndev))
		netif_wake_queue(ndev);

	return 0;
}

static int nuc980_emac_set_mac_address(struct net_device *ndev, void *addr)
{
	struct nuc980_emac *nemac = netdev_priv(ndev);
	struct sockaddr *address = addr;

	if (!is_valid_ether_addr(address->sa_data))
		return -EADDRNOTAVAIL;

	dev_addr_set(ndev, address->sa_data);
	nuc980_nemac_write_cam(nemac, 0, address->sa_data);

	return 0;
}

static int nuc980_emac_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct nuc980_emac *nemac = netdev_priv(ndev);
	struct device *dev = nemac->dev;
	struct nuc980_emac_txbd *txbd;

	txbd = &nemac->txbd[nemac->cur_tx];

	if(txbd->mode & TXBD_MODE_OWN_DMA) {
		netif_stop_queue(ndev);
		return NETDEV_TX_BUSY;
	}

	txbd->buffer = dma_map_single(dev, skb->data,
					skb->len, DMA_TO_DEVICE);

	if (dma_mapping_error(dev, txbd->buffer)) {
		dev_err(dev, "dma mapping error\n");
		return -ENOMEM;
	}

	txbd->sl = (skb->len > MAX_PACKET_SIZE) ? MAX_PACKET_SIZE : skb->len;

	dma_wmb();
	txbd->mode |= TXBD_MODE_OWN_DMA;
	dma_wmb();

	nemac->tx_skb[nemac->cur_tx] = skb;

	/* trigger tx */
	writel(TSDR_START, nemac->base + REG_TSDR);

	if (++nemac->cur_tx >= TX_QUEUE_LEN)
		nemac->cur_tx = 0;

	txbd = &nemac->txbd[nemac->cur_tx];

	if(txbd->mode & TXBD_MODE_OWN_DMA)
		netif_stop_queue(ndev);

	return NETDEV_TX_OK;
}

static irqreturn_t nuc980_emac_txirq(int irq, void *dev_id)
{
	struct nuc980_emac *nemac = dev_id;
	struct net_device *ndev = nemac->ndev;
	struct device *dev = nemac->dev;
	struct nuc980_emac_txbd *txbd;
	struct sk_buff *skb;
	unsigned long flags;
	u32 status;

	nuc980_emac_get_and_clear_int(nemac, &status, 0xffff0000);

	if (status & MISTA_EXDEF) {
		dev_err(dev, "defer exceed interrupt\n");
	} else if (status & MISTA_TXBERR) {
		dev_err(dev, "tx bus error\n");
		writel(0x00, (nemac->base + REG_MIEN));
		nemac->needs_reset = true;
		return IRQ_HANDLED;
	}

	txbd = &nemac->txbd[nemac->finish_tx];

	while ((txbd->mode & TXBD_MODE_OWN_DMA) != TXBD_MODE_OWN_DMA) {
		skb = nemac->tx_skb[nemac->finish_tx];
		if (!skb)
			break;

		dma_unmap_single(dev, txbd->buffer, skb->len, DMA_TO_DEVICE);
		txbd->buffer = 0;

		dev_kfree_skb_irq(skb);

		nemac->tx_skb[nemac->finish_tx] = NULL;

		spin_lock_irqsave(&nemac->lock, flags);
		if (txbd->sl & TXBD_SL_TXCP) {
			nemac->stats.tx_packets++;
			nemac->stats.tx_bytes += (txbd->sl & 0xffff);
		} else {
			nemac->stats.tx_errors++;
		}
		spin_unlock_irqrestore(&nemac->lock, flags);

		if (++nemac->finish_tx >= TX_QUEUE_LEN)
			nemac->finish_tx = 0;

		txbd = &nemac->txbd[nemac->finish_tx];
	}

	if (netif_queue_stopped(ndev))
		netif_wake_queue(ndev);

	return IRQ_HANDLED;
}

static int nuc980_emac_poll(struct napi_struct *napi, int budget)
{
	struct nuc980_emac *nemac =
				container_of(napi, struct nuc980_emac, napi);
	struct device *dev = nemac->dev;
	struct net_device *ndev = nemac->ndev;
	struct nuc980_emac_rxbd *rxbd;
	struct sk_buff *skb_old;
	struct sk_buff *skb_new;
	bool complete = false;
	unsigned long flags;
	unsigned status;
	unsigned len;
	int rx_cnt;

	for (rx_cnt = 0; rx_cnt < budget; rx_cnt++) {
		rxbd = &nemac->rxbd[nemac->cur_rx];

		status = rxbd->sl;

		if((status & RXBD_SL_OWN_DMA) == RXBD_SL_OWN_DMA) {
			complete = true;
			break;
		}

		skb_old = nemac->rx_skb[nemac->cur_rx];
		len = status & 0xffff;

		if (likely(status & RXBD_SL_RXGD) &&
					likely(len <= MAX_PACKET_SIZE)) {

			skb_new = netdev_alloc_skb(ndev,
					MAX_PACKET_SIZE + NET_IP_ALIGN);

			if (!skb_new) {
				dev_err(dev, "unable to allocate skb\n");
				spin_lock_irqsave(&nemac->lock, flags);
				nemac->stats.rx_dropped++;
				spin_unlock_irqrestore(&nemac->lock, flags);
			} else {
				dma_unmap_single(dev, (dma_addr_t)rxbd->buffer,
					MAX_PACKET_SIZE, DMA_FROM_DEVICE);

				skb_put(skb_old, len);
				skb_old->protocol = eth_type_trans(skb_old,
						ndev);

				netif_receive_skb(skb_old);

				spin_lock_irqsave(&nemac->lock, flags);
				nemac->stats.rx_packets++;
				nemac->stats.rx_bytes += len;
				spin_unlock_irqrestore(&nemac->lock, flags);

				skb_reserve(skb_new, NET_IP_ALIGN);
				skb_new->dev = ndev;

				rxbd->buffer = dma_map_single(dev,
						skb_new->data, MAX_PACKET_SIZE,
						DMA_FROM_DEVICE);

				nemac->rx_skb[nemac->cur_rx] = skb_new;
			}
		} else {
			spin_lock_irqsave(&nemac->lock, flags);
			nemac->stats.rx_errors++;

			if (status & RXBD_SL_RP) {
				nemac->stats.rx_length_errors++;
			} else if (status & RXBD_SL_CRCE) {
				nemac->stats.rx_crc_errors++;
			} else if (status & RXBD_SL_ALIE) {
				nemac->stats.rx_frame_errors++;
			} else if (status & RXBD_SL_PTLE) {
				nemac->stats.rx_over_errors++;
			}
			spin_unlock_irqrestore(&nemac->lock, flags);
		}

		dma_wmb();
		rxbd->sl = RXBD_SL_OWN_DMA;
		dma_wmb();

		if (++nemac->cur_rx >= RX_QUEUE_LEN)
			nemac->cur_rx = 0;
	}

	if(complete) {
		napi_complete(napi);
		if (nemac->needs_reset) {
			if (!nuc980_emac_reset_mac(nemac))
				nemac->needs_reset = false;
		}

		spin_lock_irqsave(&nemac->lock, flags);
		writel(readl(nemac->base + REG_MIEN) | MIEN_RXINTR,
					nemac->base + REG_MIEN);
		spin_unlock_irqrestore(&nemac->lock, flags);
	}

	/* trigger rx */
	writel(RSDR_START, nemac->base + REG_RSDR);
	return(rx_cnt);
}

static irqreturn_t nuc980_emac_rxirq(int irq, void *dev_id)
{
	struct nuc980_emac *nemac = dev_id;
	u32 status;

	nuc980_emac_get_and_clear_int(nemac, &status, 0xffff);

	if (unlikely(status & MISTA_RXBERR)) {
		dev_err(nemac->dev, "emc rx bus error\n");
		writel(0x00, (nemac->base + REG_MIEN));
		nemac->needs_reset = true;
		napi_schedule(&nemac->napi);
	} else {
		if(status & MISTA_RXGD) {
			writel(readl(nemac->base + REG_MIEN) & ~MIEN_RXINTR,
					nemac->base + REG_MIEN);
			napi_schedule(&nemac->napi);
		}
	}
	return IRQ_HANDLED;
}

static void nuc980_emac_handle_link_change(struct net_device *ndev)
{
	struct nuc980_emac *nemac = netdev_priv(ndev);
	struct phy_device *phydev = ndev->phydev;
	bool status_change = false;
	unsigned long flags;
	u32 val;

	if (phydev->link) {
		if (nemac->speed != phydev->speed) {
			nemac->speed = phydev->speed;
			status_change = true;
		}

		if (nemac->duplex != phydev->duplex) {
			nemac->duplex = phydev->duplex;
			status_change = true;
		}
	}

	if (nemac->link != phydev->link) {
		if (!phydev->link) {
			nemac->speed = 0;
			nemac->duplex = -1;
		}
		nemac->link = phydev->link;
		status_change = true;
	}

	if (status_change) {
		spin_lock_irqsave(&nemac->lock, flags);
		val = readl(nemac->base + REG_MCMDR);
		if (nemac->link) {
			val |= MCMDR_RXON | MCMDR_TXON;
			if (nemac->speed == 100) {
				val |= MCMDR_OPMOD;
			} else {
				val &= ~MCMDR_OPMOD;
			}

			if (nemac->duplex == DUPLEX_FULL) {
				val |= MCMDR_FDUP;
			} else {
				val &= ~MCMDR_FDUP;
			}
		} else {
			val &= ~(MCMDR_RXON | MCMDR_TXON);
		}

		writel(val, nemac->base + REG_MCMDR);
		writel(TSDR_START, nemac->base + REG_TSDR);
		spin_unlock_irqrestore(&nemac->lock, flags);

		if (nemac->link)
			dev_info(nemac->dev, "link: 1, speed: %u, duplex: %d\n",
				nemac->speed, nemac->duplex);
		else
			dev_info(nemac->dev, "link: 0\n");
	}
}

static int nuc980_emac_open(struct net_device *ndev)
{
	struct nuc980_emac *nemac = netdev_priv(ndev);
	struct device *dev = nemac->dev;
	int ret;

	ret = clk_prepare_enable(nemac->hclk);
	if (ret) {
		dev_err(dev, "unable to enable clock\n");
		return ret;
	}

	ret = nuc980_emac_reset_mac(nemac);
	if (ret)
		goto disable_clock;

	nemac->needs_reset = false;

	/* set global maccmd */
	writel(readl(nemac->base + REG_MCMDR) | MCMDR_SPCRC | MCMDR_ACP,
					nemac->base + REG_MCMDR);

	/* dma size */
	writel(MAX_PACKET_SIZE, nemac->base + REG_DMARFC);

	ret = devm_request_irq(dev, nemac->txirq, nuc980_emac_txirq, 0,
					dev_name(dev), nemac);

	if (ret) {
		dev_err(dev, "unable to request tx irq\n");
		goto free_buffers;
	}

	ret = devm_request_irq(dev, nemac->rxirq, nuc980_emac_rxirq, 0,
					dev_name(dev), nemac);

	if (ret) {
		dev_err(dev, "unable to request rx irq\n");
		goto free_tx_irq;
	}

	nemac->phydev = of_phy_connect(ndev, nemac->phy_node,
					&nuc980_emac_handle_link_change, 0,
					nemac->phy_interface);

	if (!nemac->phydev) {
		dev_err(dev, "unable to connect phy\n");
		ret = -EAGAIN;
		goto free_rx_irq;
	}

	phy_start(nemac->phydev);
	netif_start_queue(ndev);
	napi_enable(&nemac->napi);

	/* enable rx */
	writel(readl(nemac->base + REG_MCMDR) | MCMDR_RXON,
					nemac->base + REG_MCMDR);

	return 0;

free_rx_irq:
	devm_free_irq(dev, nemac->rxirq, nemac);
free_tx_irq:
	devm_free_irq(dev, nemac->txirq, nemac);
free_buffers:
	nuc980_emac_free_buffers(nemac);
disable_clock:
	clk_disable_unprepare(nemac->hclk);
	return ret;
}

static int nuc980_emac_stop(struct net_device *ndev)
{
	struct nuc980_emac *nemac = netdev_priv(ndev);
	struct device *dev = nemac->dev;

	/* disable tx and rx */
	writel(readl(nemac->base + REG_MCMDR) & ~(MCMDR_TXON | MCMDR_RXON),
						nemac->base + REG_MCMDR);

	netif_stop_queue(ndev);
	napi_disable(&nemac->napi);

	nuc980_emac_default_idle(nemac);
	nuc980_emac_free_buffers(nemac);

	phy_stop(nemac->phydev);
	phy_disconnect(nemac->phydev);

	devm_free_irq(dev, nemac->txirq, nemac);
	devm_free_irq(dev, nemac->rxirq, nemac);

	clk_disable_unprepare(nemac->hclk);

	return 0;
}

static void nuc980_emac_get_stats64(struct net_device *ndev,
					struct rtnl_link_stats64 *storage)
{
	struct nuc980_emac *nemac = netdev_priv(ndev);
	unsigned long flags;

	spin_lock_irqsave(&nemac->lock, flags);
	*storage = nemac->stats;
	spin_unlock_irqrestore(&nemac->lock, flags);
}

static void nuc980_emac_set_rx_mode(struct net_device *ndev)
{
	struct nuc980_emac *nemac = netdev_priv(ndev);
	u32 camcmr;

	if (ndev->flags & IFF_PROMISC)
		camcmr = CAMCMR_AUP | CAMCMR_AMP | CAMCMR_ABP | CAMCMR_ECMP;
	else if ((ndev->flags & IFF_ALLMULTI) || !netdev_mc_empty(ndev))
		camcmr = CAMCMR_AMP | CAMCMR_ABP | CAMCMR_ECMP;
	else
		camcmr = CAMCMR_ECMP | CAMCMR_ABP;

	writel(camcmr, nemac->base + REG_CAMCMR);
}

static int nuc980_emac_mdio_write(struct mii_bus *bus, int phy_id, int regnum,
						u16 value)
{
	struct nuc980_emac *nemac = bus->priv;
	unsigned long timeout = jiffies + msecs_to_jiffies(MII_TIMEOUT);

	writel(value, nemac->base + REG_MIID);
	writel((phy_id << 0x08) | regnum | MIIDA_PHYBUSY |
				MIIDA_MDCON | MIIDA_PHYWR,
				nemac->base + REG_MIIDA);

	while (readl(nemac->base + REG_MIIDA) & MIIDA_PHYBUSY) {
		if (time_after(jiffies, timeout)) {
			dev_dbg(nemac->dev, "mdio read timed out\n");
			return -ETIMEDOUT;
		}
		cpu_relax();
	}

	return 0;
}

static int nuc980_emac_mdio_read(struct mii_bus *bus, int phy_id, int regnum)
{
	struct nuc980_emac *nemac = bus->priv;
	unsigned long timeout = jiffies + msecs_to_jiffies(MII_TIMEOUT);

	writel((phy_id << 0x08) | regnum | MIIDA_PHYBUSY | MIIDA_MDCON,
				nemac->base + REG_MIIDA);

	while (readl(nemac->base + REG_MIIDA) & MIIDA_PHYBUSY) {
		if (time_after(jiffies, timeout)) {
			dev_dbg(nemac->dev, "mdio read timed out\n");
			return -ETIMEDOUT;
		}
		cpu_relax();
	}

	return readl(nemac->base + REG_MIID);
}

static int nuc980_emac_mdio_init(struct nuc980_emac *nemac)
{
	struct device *dev = nemac->dev;
	struct device_node *mii_np;
	int ret;

	mii_np = of_get_child_by_name(dev->of_node, "mdio-bus");
	if (!mii_np) {
		// dev_warn(dev, "mdio: no %s child node found", "mdio-bus");
		return -ENODEV;
	}

	if (!of_device_is_available(mii_np)) {
		dev_err(dev, "mdio: %s not available", "mdio-bus");
		ret = -ENODEV;
		goto put_node;
	}

	nemac->mii_bus = devm_mdiobus_alloc(dev);
	if (!nemac->mii_bus) {
		dev_err(dev, "mdio: unable to allocate memory\n");
		ret = -ENOMEM;
		goto put_node;
	}

	nemac->mclk = of_clk_get_by_name(mii_np, "hclk");
	if (IS_ERR(nemac->mclk)) {
		dev_warn(dev, "mdio: unable to get clock: hclk\n");
		nemac->mclk = NULL;
	}

	nemac->eclk = of_clk_get_by_name(mii_np, "eclk");
	if (IS_ERR(nemac->eclk)) {
		dev_warn(dev, "mdio: unable to get clock: eclk\n");
		nemac->eclk = NULL;
	}

	ret = clk_prepare_enable(nemac->mclk);
	if (ret) {
		dev_err(dev, "mdio: unable to enable clock: mclk\n");
		ret = -EINVAL;
		goto put_clocks;
	}

	ret = clk_prepare_enable(nemac->eclk);
	if (ret) {
		dev_err(dev, "mdio: unable to enable clock: eclk\n");
		ret = -EINVAL;
		goto disable_mclk;
	}

	nemac->mrst = of_reset_control_get_shared(mii_np, NULL);
	if (IS_ERR(nemac->mrst))
		nemac->mrst = NULL;

	reset_control_deassert(nemac->mrst);

	nemac->mii_bus->name = "nuc980-emac-mdio";
	nemac->mii_bus->read = nuc980_emac_mdio_read;
	nemac->mii_bus->write = nuc980_emac_mdio_write;
	nemac->mii_bus->priv = nemac;
	nemac->mii_bus->parent = dev;

	writel(readl(nemac->base + REG_MCMDR) | MCMDR_ENMDC,
				nemac->base + REG_MCMDR);

	snprintf(nemac->mii_bus->id, MII_BUS_ID_SIZE, "%s-mdio", dev_name(dev));
	ret = of_mdiobus_register(nemac->mii_bus, mii_np);
	if (ret) {
		dev_err(dev, "mdio: unable to register mdio bus\n");
		goto disable_eclk;
	}

	dev_info(dev, "mdio: initialized\n");

	return 0;

disable_eclk:
	clk_disable_unprepare(nemac->eclk);

	reset_control_put(nemac->mrst);
disable_mclk:
	clk_disable_unprepare(nemac->mclk);
put_clocks:
	clk_put(nemac->eclk);
	clk_put(nemac->mclk);
put_node:
	of_node_put(mii_np);
	return ret;
}

static const struct ethtool_ops nuc980_emac_ethtool_ops = {
	.get_link           = ethtool_op_get_link,
	.get_link_ksettings = phy_ethtool_get_link_ksettings,
	.set_link_ksettings = phy_ethtool_set_link_ksettings,
};

static const struct net_device_ops nuc980_emac_netdev_ops = {
	.ndo_open            = nuc980_emac_open,
	.ndo_stop            = nuc980_emac_stop,
	.ndo_start_xmit      = nuc980_emac_start_xmit,
	.ndo_get_stats64     = nuc980_emac_get_stats64,
	.ndo_set_rx_mode     = nuc980_emac_set_rx_mode,
	.ndo_eth_ioctl       = phy_do_ioctl_running,
	.ndo_validate_addr   = eth_validate_addr,
	.ndo_set_mac_address = nuc980_emac_set_mac_address,
};

static int nuc980_emac_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct net_device *ndev;
	struct nuc980_emac *nemac;
	bool fixed_link = false;
	u8 mac[ETH_ALEN] = {};
	int ret;

	ndev = devm_alloc_etherdev(dev, sizeof(struct nuc980_emac));
	if (!ndev) {
		dev_err(dev, "unable to allocate memory\n");
		return -ENOMEM;
	}

	nemac = netdev_priv(ndev);

	platform_set_drvdata(pdev, nemac);
	nemac->ndev = ndev;
	nemac->dev = dev;
	spin_lock_init(&nemac->lock);

	SET_NETDEV_DEV(ndev, dev);

	ret = of_address_to_resource(np, 0, &nemac->res);
	if (ret) {
		dev_err(dev, "unable to get mem region\n");
		return -EINVAL;
	}

	if (!nemac->res.start ||
			resource_size(&nemac->res) < NUC980_EMAC_REG_SIZE) {
		dev_err(dev, "mem region out of range\n");
		return -EINVAL;
	}

	dev_set_name(dev, "%08x.nuc980-emac", (unsigned int)nemac->res.start);

	nemac->hclk = devm_clk_get(dev, NULL);
	if (IS_ERR(nemac->hclk)) {
		dev_warn(dev, "unable to get clock\n");
		nemac->hclk = NULL;
	}

	nemac->rst = devm_reset_control_get_optional_shared(dev, NULL);
	if (!IS_ERR(nemac->rst))
		reset_control_deassert(nemac->rst);

	nemac->base = devm_ioremap_resource(dev, &nemac->res);
	if (IS_ERR(nemac->base)) {
		dev_err(dev, "unable to map mem region\n");
		return PTR_ERR(nemac->base);
	}

	nemac->txirq = of_irq_get_byname(np, "tx");
	if (nemac->txirq < 0) {
		dev_err(dev, "unable to get tx irq\n");
		return -EINVAL;
	}

	nemac->rxirq = of_irq_get_byname(np, "rx");
	if (nemac->rxirq < 0) {
		dev_err(dev, "unable to get rx irq\n");
		return -EINVAL;
	}

	nemac->phy_node = of_parse_phandle(np, "phy-handle", 0);

	if (!nemac->phy_node && of_phy_is_fixed_link(np)) {
		ret = of_phy_register_fixed_link(np);
		if (ret < 0) {
			dev_err(dev, "unable to register fixed PHY\n");
			return ret;
		};

		nemac->phy_node = of_node_get(np);
		dev_info(dev, "emac: fixed-link detected\n");
		fixed_link = true;
	}

	if (!nemac->phy_node) {
		dev_err(dev, "unable to get phy\n");
		return -EINVAL;
	}

	ret = of_get_mac_address(np, mac);
	if (ret == -EPROBE_DEFER) {
		ret = driver_deferred_probe_check_state(dev);
		if (ret == -EPROBE_DEFER)
			goto put_phy_node;
	}

	if (!is_valid_ether_addr(mac)) {
		eth_hw_addr_random(ndev);
		dev_warn(dev, "using random MAC address: %pM\n",
							ndev->dev_addr);
	} else {
		dev_addr_set(ndev, mac);
	}

	ndev->tx_queue_len = TX_QUEUE_LEN;
	ndev->netdev_ops = &nuc980_emac_netdev_ops;
	ndev->ethtool_ops = &nuc980_emac_ethtool_ops;

	netif_napi_add_weight(ndev, &nemac->napi, nuc980_emac_poll, RX_QUEUE_LEN / 4);

	ether_setup(ndev);
	ndev->max_mtu = MAX_PACKET_SIZE;
	netif_carrier_off(ndev);

	ret = register_netdev(ndev);
	if (ret) {
		dev_err(dev, "unable to register netdev\n");
		goto free_napi;
	}

	nuc980_emac_mdio_init(nemac);

	dev_info(dev, "emac: initialized\n");

	return 0;

put_phy_node:
	of_node_put(nemac->phy_node);
	if (fixed_link)
		of_phy_deregister_fixed_link(np);
	return ret;

free_napi:
	netif_napi_del(&nemac->napi);
	goto put_phy_node;
}

static const struct of_device_id nuc980_emac_dt_ids[] = {
	{ .compatible = "nuvoton,nuc980-emac" },
	{},
};

static struct platform_driver nuc980_emac_platform_driver = {
	.probe  = nuc980_emac_probe,
	.driver = {
		.name           = "nuc980-emac",
		.of_match_table = nuc980_emac_dt_ids,
	},
};

static int __init nuc980_emac_init(void)
{
	return platform_driver_register(&nuc980_emac_platform_driver);
}
device_initcall(nuc980_emac_init);
