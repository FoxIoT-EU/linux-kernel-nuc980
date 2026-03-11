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
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/usb/gadget.h>
#include <linux/dma-mapping.h>

#define REG_GINTSTS           0x000
#define   GINTSTS_USBIF       (1 << 0)
#define   GINTSTS_CEPIF       (1 << 1)
#define   GINTSTS_EPIF        (0xfff << 2)
#define REG_GINTEN            0x008
#define   GINTEN_USBIEN       (1 << 0)
#define   GINTEN_CEPIEN       (1 << 1)
#define REG_BUSINTSTS         0x010
#define   BUSINTSTS_RSTIF     (1 << 1)
#define   BUSINTSTS_RESUMEIF  (1 << 2)
#define   BUSINTSTS_SUSPENDIF (1 << 3)
#define   BUSINTSTS_HISPDIF   (1 << 4)
#define   BUSINTSTS_DMADONEIF (1 << 5)
#define   BUSINTSTS_VBUSDETIF (1 << 8)
#define REG_BUSINTEN          0x014
#define   BUSINTEN_RSTIEN     (1 << 1)
#define   BUSINTEN_RESUMEIEN  (1 << 2)
#define   BUSINTEN_SUSPENDIEN (1 << 3)
#define   BUSINTEN_DMADONEIEN (1 << 5)
#define   BUSINTEN_VBUSDETIEN (1 << 8)
#define REG_OPER              0x018
#define REG_FRAMECNT          0x01c
#define REG_FADDR             0x020
#define REG_TEST              0x024
#define REG_CEPDAT            0x028
#define REG_CEPCTL            0x02c
#define   CEPCTL_FLUSH        (1 << 3)
#define   CEPCTL_STALLEN      (1 << 1)
#define REG_CEPINTEN          0x030
#define   CEPINTEN_SETUPPKIEN (1 << 1)
#define   CEPINTEN_INTKIEN    (1 << 3)
#define   CEPINTEN_TXPKIEN    (1 << 5)
#define   CEPINTEN_RXPKIEN    (1 << 6)
#define   CEPINTEN_STSDONEIEN (1 << 10)
#define REG_CEPINTSTS         0x034
#define   CEPINTSTS_SETUPPKIF (1 << 1)
#define   CEPINTSTS_INTKIF    (1 << 3)
#define   CEPINTSTS_RXPKIF    (1 << 6)
#define   CEPINTSTS_STSDONEIF (1 << 10)
#define REG_CEPTXCNT          0x038
#define REG_CEPDATCNT         0x040
#define REG_SETUP1_0          0x044
#define REG_SETUP3_2          0x048
#define REG_SETUP5_4          0x04c
#define REG_SETUP7_6          0x050
#define REG_CEPBUFSTART       0x054
#define REG_CEPBUFEND         0x058
#define REG_DMACTL            0x05c
#define REG_DMACNT            0x060
#define REG_EP_INTSTS         0x068
#define   EP_INTSTS_TXPKIF    (1 << 3)
#define   EP_INTSTS_RXPKIF    (1 << 4)
#define   EP_INTSTS_INTKIF    (1 << 6)
#define   EP_INTSTS_SHORTRXIF (1 << 12)
#define REG_EP_INTEN          0x06c
#define REG_EP_DATCNT         0x070
#define REG_EP_RSPCTL         0x074
#define   EP_RSPCTL_FLUSH     (1 << 0)
#define   EP_RSPCTL_MODE_AUTO (0x00 << 1)
#define   EP_RSPCTL_MODE_MANUAL (0x01 << 1)
#define   EP_RSPCTL_TOGGLE    (1 << 3)
#define   EP_RSPCTL_HALT      (1 << 4)
#define   EP_RSPCTL_ZEROLEN   (1 << 5)
#define   EP_RSPCTL_SHORTTXEN (1 << 6)
#define REG_EP_MPS            0x078
#define REG_EP_TXCNT          0x07c
#define REG_EP_CFG            0x080
#define   EP_CFG_EPEN         (1 << 0)
#define   EP_CFG_EPTYPE_BULK  (0x01 << 1)
#define   EP_CFG_EPTYPE_INT   (0x10 << 1)
#define   EP_CFG_EPTYPE_ISO   (0x11 << 1)
#define REG_EP_BUFSTART       0x084
#define REG_EP_BUFEND         0x088
#define REG_DMAADDR           0x700
#define REG_PHYCTL            0x704
#define   PHYCTL_DPPUEN       (1 << 8)
#define   PHYCTL_PHYEN        (1 << 9)
#define   PHYCTL_VBUSDET      (1 << 31)
#define NUC980_UDC_REG_SIZE   0x1000

#define NUM_ENDPOINTS         13
#define USBD_TIMEOUT	      10000
#define EP_NAME_SIZE          6
#define EP0_FIFO_SIZE         64
#define EP_FIFO_SIZE          512

#define EP0_IDLE              0
#define EP0_IN_DATA_PHASE     1
#define EP0_OUT_DATA_PHASE    2
#define EP0_END_XFER          3
#define EP0_STALL             4

#define to_nuc980_udc(g)      (container_of((g), struct nuc980_udc, gadget))
#define nepreg(nudc, reg, i)  (nudc->base + reg + 0x28 * (i - 1))
#define epreg(ep, reg)        (ep->nudc->base + reg + 0x28 * (ep->index - 1))

struct nuc980_ep {
	struct list_head  queue;
	struct nuc980_udc *nudc;
	struct usb_ep     ep;
	char              name[EP_NAME_SIZE];
	u8                index;
	u8                buffer_disabled;
	u8                endpoint_address;
	u8                ep_mode;
	u8                ep_num;
	u8                ep_dir;
	u8                ep_type;
	u32               irq_enb;
};

struct nuc980_request {
	struct list_head   queue;
	struct usb_request req;
	u32                dma_mapped;
};

struct nuc980_udc {
	struct device            *dev;
	void __iomem             *base;
	struct clk               *clk;
	spinlock_t               lock;
	int                      ep0state;
	struct usb_gadget        gadget;
	struct usb_gadget_driver *driver;
	u8                       usb_address;
	u8                       usb_less_mps;
	s32                      setup_ret;
	u32                      irq_enbl;
	u32                      usb_dma_cnt;
//	u8                       usb_dma_dir;
	u8                       usb_dma_trigger;
	u8                       usb_dma_trigger_next;
	u32                      usb_dma_owner;
	struct nuc980_ep         ep[NUM_ENDPOINTS];
	struct usb_ctrlrequest   crq;
	s32                      sram_data[13][2];
	void                     *dma_vaddr;
	dma_addr_t               dma_paddr;
};

static void nuc980_udc_enable(struct nuc980_udc *nudc)
{
	writel(readl(nudc->base + REG_PHYCTL) | PHYCTL_DPPUEN,
			nudc->base + REG_PHYCTL);
}

static void nuc980_udc_disable(struct nuc980_udc *nudc)
{
	int i;

	writel(0, nudc->base + REG_CEPINTEN);
	writel(0xffff, nudc->base + REG_CEPINTSTS);
	writel(readl(nudc->base + REG_CEPCTL) | CEPCTL_FLUSH,
			nudc->base + REG_CEPCTL);

	for (i = 1; i < NUM_ENDPOINTS; i++)
		writel(EP_RSPCTL_FLUSH | EP_RSPCTL_TOGGLE,
			nepreg(nudc, REG_EP_RSPCTL, i));

	writel(readl(nudc->base + REG_PHYCTL) & ~PHYCTL_DPPUEN,
			nudc->base + REG_PHYCTL);

	nudc->gadget.speed = USB_SPEED_UNKNOWN;
}

static void nuc980_udc_request_done(struct nuc980_ep *ep,
					struct nuc980_request *req, int status)
{
	struct nuc980_udc *nudc = ep->nudc;

	list_del_init(&req->queue);
	if (ep->index) {
		if (list_empty(&ep->queue)) {
			writel(0, epreg(ep, REG_EP_INTEN));
		} else {
			writel(ep->irq_enb, epreg(ep, REG_EP_INTEN));
		}
	}

	if (req->req.status == -EINPROGRESS)
		req->req.status = status;

	usb_gadget_unmap_request(&nudc->gadget, &req->req, ep->ep_dir);
	req->req.complete(&ep->ep, &req->req);
}

static void nuc980_udc_request_clear(struct nuc980_udc *nudc,
					struct nuc980_ep *ep, int status)
{
	struct nuc980_request *req;

	while (!list_empty (&ep->queue)) {
		req = list_entry(ep->queue.next, struct nuc980_request, queue);
		nuc980_udc_request_done(ep, req, status);
	}
}

static int nuc980_udc_read_packet(struct nuc980_ep *ep, u8 *buf,
					struct nuc980_request *req, u16 cnt)
{
	struct nuc980_udc *nudc = ep->nudc;
	unsigned int timeout;
	unsigned int data;
	int i;

	if (ep->ep_num == 0) {
		for (i = 0; i < cnt; i++) {
			data = readb(nudc->base + REG_CEPDAT);
			*buf++ = data & 0xFF;
		}
		req->req.actual += cnt;
	} else {
		usb_gadget_map_request(&nudc->gadget, &req->req, ep->ep_dir);

		writel((readl(nudc->base + REG_DMACTL) & 0xe0) | ep->ep_num,
					nudc->base + REG_DMACTL);
		writel(nudc->dma_paddr, nudc->base + REG_DMAADDR);
		writel(cnt, nudc->base + REG_DMACNT);
		writel(0x20, nudc->base + REG_BUSINTSTS);
		writel(readl(nudc->base + REG_DMACTL) | 0x20,
					nudc->base + REG_DMACTL);

		timeout = 0;
		while (!(readl(nudc->base + REG_BUSINTSTS) & 0x20)) {
			if (!(readl(nudc->base + REG_PHYCTL) & PHYCTL_VBUSDET))
				break;

			if (timeout > USBD_TIMEOUT) {
				writel(0x80, nudc->base + REG_DMACTL);
				writel(0x00, nudc->base + REG_DMACTL);
				writel(readl(nudc->base + REG_CEPCTL) | 
						CEPCTL_FLUSH,
						nudc->base + REG_CEPCTL);
				writel(EP_RSPCTL_FLUSH | EP_RSPCTL_TOGGLE,
						epreg(ep, REG_EP_RSPCTL));
				break;
			}
			timeout++;
		}
		writel(0x20, nudc->base + REG_BUSINTSTS);
		memcpy(buf, nudc->dma_vaddr, cnt);
		req->req.actual += cnt;
	}

	return cnt;
}

static int nuc980_udc_read_fifo(struct nuc980_ep *ep,
					struct nuc980_request *req, u16 cnt)
{
	unsigned bufferspace;
	int fifo_count = 0;
	int is_last = 1;
	u8 *buf;

	buf = req->req.buf + req->req.actual;
	bufferspace = req->req.length - req->req.actual;

	if (cnt > ep->ep.maxpacket)
		cnt = ep->ep.maxpacket;
	if (cnt > bufferspace) {
		req->req.status = -EOVERFLOW;
		cnt = bufferspace;
	}

	fifo_count = nuc980_udc_read_packet(ep, buf, req, cnt);

	if (req->req.length == req->req.actual) {
		nuc980_udc_request_done(ep, req, 0);
	} else if (fifo_count && fifo_count < ep->ep.maxpacket) {
		nuc980_udc_request_done(ep, req, 0);
	} else {
		is_last = 0;
	}

	return is_last;
}

static int nuc980_udc_write_packet(struct nuc980_ep *ep,
					struct nuc980_request *req)
{
	struct nuc980_udc *nudc = ep->nudc;
	unsigned int timeout;
	unsigned int total;
	unsigned int len;
	u32 val;
	u8 *buf;
	int i;

	buf = req->req.buf + req->req.actual;
	prefetch(buf);
	total = req->req.length - req->req.actual;
	if (ep->ep.maxpacket < total)
		len = ep->ep.maxpacket;
	else
		len = total;

	if (ep->ep_num == 0) {
		for (i = 0; i < len; i++) {
			writeb(*buf++ & 0xff, nudc->base + REG_CEPDAT);
		}
		writel(len, nudc->base + REG_CEPTXCNT);
		req->req.actual += len;
	} else {
		usb_gadget_map_request(&nudc->gadget, &req->req, ep->ep_dir);
		buf = req->req.buf + req->req.actual;

		if (len == 0) {
			writel(EP_RSPCTL_ZEROLEN, epreg(ep, REG_EP_RSPCTL));
		} else {
			memcpy(nudc->dma_vaddr, buf, len);
			writel((readl(nudc->base + REG_DMACTL) & 0xe0) |
				0x110 | ep->ep_num, nudc->base + REG_DMACTL);
			writel(0, epreg(ep, REG_EP_INTEN));
			writel((BUSINTEN_DMADONEIEN | BUSINTEN_RSTIEN | 
				BUSINTEN_SUSPENDIEN | BUSINTEN_VBUSDETIEN),
				nudc->base + REG_BUSINTEN);
			writel(nudc->dma_paddr, nudc->base + REG_DMAADDR);
			writel(len, nudc->base + REG_DMACNT);
			writel(0x20, nudc->base + REG_BUSINTSTS);

			writel(readl(nudc->base + REG_DMACTL) | 0x20,
				nudc->base + REG_DMACTL);
			timeout = 0;
			while (!(readl(nudc->base + REG_BUSINTSTS) & 0x20)) {
				if (!(readl(nudc->base + REG_PHYCTL) & PHYCTL_VBUSDET))
					break;

				if (timeout > USBD_TIMEOUT) {
					writel(0x80, nudc->base + REG_DMACTL);
					writel(0x00, nudc->base + REG_DMACTL);
					writel(readl(nudc->base + REG_CEPCTL) |
						CEPCTL_FLUSH,
						nudc->base + REG_CEPCTL);
					writel(EP_RSPCTL_FLUSH |
						EP_RSPCTL_TOGGLE,
						epreg(ep, REG_EP_RSPCTL));
					break;
				}
				timeout++;
			}
			writel(0x20, nudc->base + REG_BUSINTSTS);
		}
		val = readl(epreg(ep, REG_EP_RSPCTL));
		writel((val & 0x16) | EP_RSPCTL_SHORTTXEN, epreg(ep, REG_EP_RSPCTL));
		writel(len, epreg(ep, REG_EP_TXCNT));
		req->req.actual += len;
	}

	return len;
}

static int nuc980_udc_write_fifo(struct nuc980_ep *ep,
					struct nuc980_request *req)
{
	u32 len;

	len = nuc980_udc_write_packet(ep, req);

	if (req->req.length == req->req.actual) {
		nuc980_udc_request_done(ep, req, 0);
		return 1;
	}

	return 0;
}

static void nuc980_udc_rst_irq(struct nuc980_udc *nudc)
{
	int i;

	nuc980_udc_request_clear(nudc, &nudc->ep[0], -ECONNRESET);

	nudc->usb_address = 0;
	nudc->usb_less_mps = 0;

	writel(0x80, nudc->base + REG_DMACTL);
	writel(0x00, nudc->base + REG_DMACTL);

	if (readl(nudc->base + REG_OPER) & 0x04)
		nudc->gadget.speed = USB_SPEED_HIGH;
	else
		nudc->gadget.speed = USB_SPEED_FULL;

	writel(readl(nudc->base + REG_CEPCTL) | CEPCTL_FLUSH,
				nudc->base + REG_CEPCTL);

	for (i = 1; i < NUM_ENDPOINTS; i++)
		writel(EP_RSPCTL_FLUSH | EP_RSPCTL_TOGGLE,
				nepreg(nudc, REG_EP_RSPCTL, i));

	writel(0, nudc->base + REG_FADDR);
	writel(CEPINTEN_SETUPPKIEN, nudc->base + REG_CEPINTEN);
}

static void nuc980_udc_dma_irq(struct nuc980_udc *nudc)
{
	struct nuc980_request *req;
	struct nuc980_ep *ep;
	u32 val;

	if (!nudc->usb_dma_trigger)
		return;

	ep = &nudc->ep[nudc->usb_dma_owner];

//	if (nudc->usb_dma_dir == Ep_In)
//		writel(EP_INTSTS_INTKIF, epreg(ep, REG_EP_INTSTS));

	nudc->usb_dma_trigger = 0;
	if (list_empty(&ep->queue)) {
		req = NULL;
		writel(nudc->irq_enbl, nudc->base + REG_GINTEN);
		return;
	} else {
		req = list_entry(ep->queue.next, struct nuc980_request, queue);
	}

	if (req) {
		if (ep->ep_type == EP_CFG_EPTYPE_BULK) {
			if (nudc->usb_less_mps == 1) {
				val = readl(epreg(ep, REG_EP_RSPCTL));
				writel((val & 0x16) | EP_RSPCTL_SHORTTXEN,
					epreg(ep, REG_EP_RSPCTL));
				nudc->usb_less_mps = 0;
			}
		} else if (ep->ep_type == EP_CFG_EPTYPE_INT) {
			writel(nudc->usb_dma_cnt, epreg(ep, REG_EP_TXCNT));
		} else if (ep->ep_type == EP_CFG_EPTYPE_ISO) {
			if (nudc->usb_less_mps == 1) {
				val = readl(epreg(ep, REG_EP_RSPCTL));
				writel((val & 0x16) | EP_RSPCTL_SHORTTXEN,
					epreg(ep, REG_EP_RSPCTL));
				nudc->usb_less_mps = 0;
			}
		}
		req->req.actual += nudc->usb_dma_cnt;
		if ((req->req.length == req->req.actual) ||
					nudc->usb_dma_cnt < ep->ep.maxpacket) {
			writel(nudc->irq_enbl, nudc->base + REG_GINTEN);
			if ((ep->ep_type == EP_CFG_EPTYPE_BULK) &&
					(ep->ep_dir == 0) &&
					(nudc->usb_dma_cnt < ep->ep.maxpacket)) {
				if (ep->buffer_disabled) {
					val = readl(epreg(ep, REG_EP_RSPCTL));

					writel(val & 0x16, epreg(ep, REG_EP_RSPCTL));
					writel((val & 0x16) | 0x80, epreg(ep, REG_EP_RSPCTL));
				}
			}
			nuc980_udc_request_done(ep, req, 0);
			return;
		}
	}

//	if (nudc->usb_dma_dir == Ep_Out) {
		if (nudc->usb_dma_trigger_next) {
			nudc->usb_dma_trigger_next = 0;
			nuc980_udc_read_fifo(ep, req, 0);
		}
/*	} else if (nudc->usb_dma_dir == Ep_In) {
		if (nudc->usb_less_mps == 1)
			nudc->usb_less_mps = 0;

		if (nudc->usb_dma_trigger_next) {
			nudc->usb_dma_trigger_next = 0;
			write_fifo(ep, req);
		}
	}*/
}

static void nuc980_udc_bus_irq(struct nuc980_udc *nudc, u32 st)
{
	if (st & BUSINTSTS_RSTIF)
		nuc980_udc_rst_irq(nudc);

	if (st & BUSINTSTS_RESUMEIF)
		writel((BUSINTEN_RSTIEN | BUSINTEN_SUSPENDIEN | BUSINTEN_VBUSDETIEN),
					nudc->base + REG_BUSINTEN);

	if (st & BUSINTSTS_SUSPENDIF)
		writel((BUSINTEN_RSTIEN | BUSINTEN_RESUMEIEN | BUSINTEN_VBUSDETIEN),
					nudc->base + REG_BUSINTEN);

	if (st & BUSINTSTS_HISPDIF) {
		nudc->gadget.speed = USB_SPEED_HIGH;
		nudc->usb_address = 0;
		writel(CEPINTEN_SETUPPKIEN, nudc->base + REG_CEPINTEN);
	}

	if (st & BUSINTSTS_DMADONEIF)
		nuc980_udc_dma_irq(nudc);

	if (st & BUSINTSTS_VBUSDETIF) {
		if (readl(nudc->base + REG_PHYCTL) & PHYCTL_VBUSDET) {
			writel(readl(nudc->base + REG_CEPCTL) | CEPCTL_FLUSH,
					nudc->base + REG_CEPCTL);
			nuc980_udc_enable(nudc);
		} else {
			nuc980_udc_disable(nudc);
			nuc980_udc_request_clear(nudc, &nudc->ep[0], -ESHUTDOWN);
		}
	}
}

static void nuc980_udc_ctrl_irq(struct nuc980_udc *nudc)
{
	struct nuc980_ep *ep = &nudc->ep[0];
	struct usb_ctrlrequest crq;
	struct nuc980_request *req;
	int ret;

	if (list_empty(&ep->queue)) {
		req = NULL;
	} else {
		req = list_entry(ep->queue.next, struct nuc980_request, queue);
	}

	crq.bRequestType = (u8)readl(nudc->base + REG_SETUP1_0) & 0xff;
	crq.bRequest = (u8)(readl(nudc->base + REG_SETUP1_0) >> 8) & 0xff;
	crq.wValue = (u16)readl(nudc->base + REG_SETUP3_2);
	crq.wIndex = (u16)readl(nudc->base + REG_SETUP5_4);
	crq.wLength = (u16)readl(nudc->base + REG_SETUP7_6);
	nudc->crq = crq;

	if (nudc->ep0state == EP0_IDLE) {
		if (crq.bRequest == USB_REQ_SET_ADDRESS) {
			nudc->usb_address = crq.wValue;
		}

		if (crq.bRequestType & USB_DIR_IN) {
			nudc->ep0state = EP0_IN_DATA_PHASE;
			writel(CEPINTEN_SETUPPKIEN | CEPINTEN_INTKIEN,
					nudc->base + REG_CEPINTEN);
		} else {
			nudc->ep0state = EP0_OUT_DATA_PHASE;
			writel(CEPINTEN_SETUPPKIEN | CEPINTEN_RXPKIEN,
					nudc->base + REG_CEPINTEN);
		}

		if (nudc->gadget.speed == USB_SPEED_FULL)
			udelay(5);

		ret = nudc->driver->setup(&nudc->gadget, &crq);
		nudc->setup_ret = ret;
		if ((ret < 0) || (crq.bRequest == USB_REQ_SET_ADDRESS)) {
			writel(CEPINTSTS_STSDONEIF, nudc->base + REG_CEPINTSTS);
			writel(0, nudc->base + REG_CEPCTL);
			writel(CEPINTEN_SETUPPKIEN | CEPINTEN_STSDONEIEN,
					nudc->base + REG_CEPINTEN);
		} else if (ret > 1000) {
			nudc->ep0state = EP0_END_XFER;
			writel(CEPINTEN_SETUPPKIEN, nudc->base + REG_CEPINTEN);
		}
	}
}

static void nuc980_udc_cep_irq(struct nuc980_udc *nudc, u32 st)
{
	struct nuc980_ep *ep = &nudc->ep[0];
	struct nuc980_request *req;
	unsigned int timeout;
	int is_last = 1;

	if (list_empty(&ep->queue))
		req = NULL;
	else
		req = list_entry(ep->queue.next, struct nuc980_request, queue);

	if (st & CEPINTSTS_SETUPPKIF) {
		nudc->ep0state = EP0_IDLE;
		nudc->setup_ret = 0;
		nuc980_udc_ctrl_irq(nudc);
	}

	if (st & CEPINTSTS_RXPKIF) {
		if (nudc->ep0state == EP0_OUT_DATA_PHASE) {
			if (req)
				is_last = nuc980_udc_read_fifo(ep, req, 
					readl(nudc->base + REG_CEPDATCNT));

			writel(CEPINTSTS_STSDONEIF, nudc->base + REG_CEPINTSTS);
			if (!is_last) {
				writel(CEPINTEN_SETUPPKIEN | CEPINTEN_RXPKIEN |
					CEPINTEN_STSDONEIEN,
					nudc->base + REG_CEPINTEN);
			} else {
				writel(0, nudc->base + REG_CEPCTL);
				writel(CEPINTEN_SETUPPKIEN | CEPINTEN_STSDONEIEN,
					nudc->base + REG_CEPINTEN);
				nudc->ep0state = EP0_END_XFER;
			}
		}
	}

	if (st & CEPINTSTS_INTKIF) {
		if (nudc->ep0state == EP0_IN_DATA_PHASE) {
			timeout = 0;
			while (1) {
				if (readl(nudc->base + REG_CEPINTSTS) & 0x1000)
					break;
				if (timeout > USBD_TIMEOUT)
					return;

				timeout++;
			}

			if (req)
				is_last = nuc980_udc_write_fifo(ep, req);

			if (!is_last) {
				writel(CEPINTEN_SETUPPKIEN | CEPINTEN_STSDONEIEN |
					CEPINTEN_TXPKIEN | CEPINTEN_INTKIEN,
					nudc->base + REG_CEPINTEN);
			} else {
				if (nudc->setup_ret >= 0)
					writel(0, nudc->base + REG_CEPCTL);

				writel(CEPINTEN_STSDONEIEN | CEPINTEN_TXPKIEN |
					CEPINTEN_SETUPPKIEN,
					nudc->base + REG_CEPINTEN);

				if (nudc->setup_ret < 0)
					nudc->ep0state = EP0_IDLE;
				else if (nudc->ep0state != EP0_IDLE)
					nudc->ep0state = EP0_END_XFER;
			}
		}
	}

	if (st & CEPINTSTS_STSDONEIF) {
		writel(CEPINTEN_SETUPPKIEN, nudc->base + REG_CEPINTEN);
		if (nudc->crq.bRequest == USB_REQ_SET_ADDRESS) {
			writel(nudc->usb_address, nudc->base + REG_FADDR);
		} else if (nudc->crq.bRequest == USB_REQ_SET_FEATURE) {
			if ((nudc->crq.bRequestType & 0x3) == 0x0) {
				if ((nudc->crq.wValue & 0x3) == 0x2) {
					writel(nudc->crq.wIndex >> 8,
						nudc->base + REG_TEST);
				}
			}
		}
		nudc->ep0state = EP0_IDLE;
		nudc->setup_ret = 0;
	}
}

static void nuc980_udc_ep_irq(struct nuc980_ep *ep, u32 st)
{
	struct nuc980_udc *nudc = ep->nudc;
	struct nuc980_request *req;
	unsigned int timeout;

	if (list_empty(&ep->queue)) {
		req = NULL;
	} else {
		req = list_entry(ep->queue.next, struct nuc980_request, queue);
	}

	if (st & EP_INTSTS_INTKIF) {
		writel(EP_INTSTS_INTKIF, epreg(ep, REG_EP_INTSTS));
		if (ep->ep_type == EP_CFG_EPTYPE_BULK) {
			if (readl(epreg(ep, REG_EP_RSPCTL)) &
							EP_RSPCTL_SHORTTXEN)
				return;
		}

		if (req == NULL) {
			writel(0, epreg(ep, REG_EP_INTEN));
			return;
		}

		timeout = 0;
		while (readl(nudc->base + REG_DMACTL) & 0x20) {
			if (!(readl(nudc->base + REG_PHYCTL) & PHYCTL_VBUSDET)) {
				return;
			}
			if (timeout > USBD_TIMEOUT) {
				writel(0x80, nudc->base + REG_DMACTL);
				writel(0x00, nudc->base + REG_DMACTL);
				writel(readl(nudc->base + REG_CEPCTL) |
						CEPCTL_FLUSH,
						nudc->base + REG_CEPCTL);
				writel(EP_RSPCTL_FLUSH | EP_RSPCTL_TOGGLE,
						epreg(ep, REG_EP_RSPCTL));
				return;
			}
			timeout++;
		}
		if (!nuc980_udc_write_fifo(ep, req))
			writel(0x40, epreg(ep, REG_EP_INTEN));
	}

	if (st & EP_INTSTS_TXPKIF)
		writel(EP_INTSTS_TXPKIF, epreg(ep, REG_EP_INTSTS));

	if ((st & EP_INTSTS_RXPKIF) || (st & EP_INTSTS_SHORTRXIF)) {
		writel(EP_INTSTS_RXPKIF | EP_INTSTS_SHORTRXIF,
				epreg(ep, REG_EP_INTSTS));
		if (req == NULL) {
			writel(0, epreg(ep, REG_EP_INTEN));
			writel(st, epreg(ep, REG_EP_INTSTS));
			return;
		}
		nuc980_udc_read_fifo(ep, req, readl(epreg(ep, REG_EP_DATCNT)));
	}
}

static irqreturn_t nuc980_udc_irq(int irq, void *dev_id)
{
	struct nuc980_udc *nudc = dev_id;
	u32 stl;
	u32 st;
	int i;

	stl = readl(nudc->base + REG_GINTSTS) & readl(nudc->base + REG_GINTEN);
	if (!stl)
		return IRQ_NONE;

	if (stl & GINTSTS_USBIF) {
		st = readl(nudc->base + REG_BUSINTSTS) &
				readl(nudc->base + REG_BUSINTEN);
		writel(st, nudc->base + REG_BUSINTSTS);
		if (st && nudc->driver)
			nuc980_udc_bus_irq(nudc, st);
	}

	if (stl & GINTSTS_CEPIF) {
		st = readl(nudc->base + REG_CEPINTSTS) &
				readl(nudc->base + REG_CEPINTEN);
		writel(st, nudc->base + REG_CEPINTSTS);
		if (st && nudc->driver)
			nuc980_udc_cep_irq(nudc, st);
	}

	if (stl & GINTSTS_EPIF) {
		stl >>= 1;
		for (i = 1; i < NUM_ENDPOINTS; i++) {
			if (stl & (1 << i)) {
				st = readl(nepreg(nudc, REG_EP_INTSTS, i)) &
					readl(nepreg(nudc, REG_EP_INTEN, i));
				if (st && nudc->driver)
					nuc980_udc_ep_irq(&nudc->ep[i], st);
			}
		}
	}

	return IRQ_HANDLED;
}

static int nuc980_udc_get_frame(struct usb_gadget *gadget)
{
	struct nuc980_udc *nudc = to_nuc980_udc(gadget);
	int val;

	val = readl(nudc->base + REG_FRAMECNT);
	return val & 0xffff;
}

static int nuc980_udc_wakeup(struct usb_gadget *gadget)
{
	return 0;
}

static int nuc980_udc_set_selfpowered(struct usb_gadget *gadget, int value)
{
	return 0;
}

static int nuc980_udc_pullup(struct usb_gadget *gadget, int is_on)
{
	struct nuc980_udc *nudc = to_nuc980_udc(gadget);

	if (is_on) {
		nuc980_udc_enable(nudc);
	} else {
		if (nudc->gadget.speed != USB_SPEED_UNKNOWN) {
			if (nudc->driver && nudc->driver->disconnect)
				nudc->driver->disconnect(&nudc->gadget);
		}
		nuc980_udc_disable(nudc);
	}
	return 0;
}

static int nuc980_udc_start(struct usb_gadget *gadget,
					struct usb_gadget_driver *driver)
{
	struct nuc980_udc *nudc = to_nuc980_udc(gadget);

	nudc->driver = driver;
	nudc->usb_address = 0;
	nudc->gadget.dev.of_node = nudc->dev->of_node;
	nudc->gadget.is_selfpowered = 1;

	writel(0x03, nudc->base + REG_GINTEN);
	writel((BUSINTEN_RESUMEIEN | BUSINTEN_RSTIEN | BUSINTEN_VBUSDETIEN),
					nudc->base + REG_BUSINTEN);
	writel(0, nudc->base + REG_FADDR);
	writel(0x402, nudc->base + REG_CEPINTEN);

	nuc980_udc_enable(nudc);

	return 0;
}

static int nuc980_udc_stop(struct usb_gadget *gadget)
{
	struct nuc980_udc *nudc = to_nuc980_udc(gadget);
	int i;

	nudc->driver = NULL;

	writel(0, nudc->base + REG_BUSINTEN);
	writel(0xffff, nudc->base + REG_BUSINTSTS);

	writel(0, nudc->base + REG_CEPINTEN);
	writel(0xffff, nudc->base + REG_CEPINTSTS);

	for (i = 1; i < NUM_ENDPOINTS; i++) {
		writel(0, nepreg(nudc, REG_EP_INTEN, i));
		writel(0xffff, nepreg(nudc, REG_EP_INTSTS, i));
	}

	nuc980_udc_disable(nudc);

	return 0;
}

static s32 nuc980_udc_get_sram_base(struct nuc980_udc *nudc, u32 max)
{
	struct nuc980_ep *ep;
	s32 start;
	s32 end;
	int cnt = 1;
	int i;
	int j;

	for (i = 1; i < NUM_ENDPOINTS; i++) {
		ep = &nudc->ep[i];

		start = readl(epreg(ep, REG_EP_BUFSTART));
		end = readl(epreg(ep, REG_EP_BUFEND));
		if (end - start > 0) {
				nudc->sram_data[cnt][0] = start;
				nudc->sram_data[cnt][1] = end + 1;
				cnt++;
		}
	}

	if (cnt == 1)
		return 0x40;

	j = 1;
	while (j < cnt) {
		for (i = 0; i < cnt - j; i++) {
			if (nudc->sram_data[i][0] > nudc->sram_data[i + 1][0]) {
				start = nudc->sram_data[i][0];
				end = nudc->sram_data[i][1];
				nudc->sram_data[i][0] = nudc->sram_data[i + 1][0];
				nudc->sram_data[i][1] = nudc->sram_data[i + 1][1];
				nudc->sram_data[i + 1][0] = start;
				nudc->sram_data[i + 1][1] = end;
			}
		}
		j++;
	}

	for (i = 0; i < cnt - 1; i++) {
		if (nudc->sram_data[i + 1][0] - nudc->sram_data[i][1] >= max)
			return nudc->sram_data[i][1];
	}

	if (0x1000 - nudc->sram_data[cnt - 1][1] >= max)
		return nudc->sram_data[cnt - 1][1];

	return -ENOBUFS;
}

static int nuc980_udc_ep_enable(struct usb_ep *uep,
				const struct usb_endpoint_descriptor *desc)
{
	struct nuc980_ep *ep = container_of(uep, struct nuc980_ep, ep);
	struct nuc980_udc *nudc = ep->nudc;
	unsigned long flags;
	u32 int_en_reg;
	s32 sram_addr;
	u32 max;

	if (!uep || !desc || (desc->bDescriptorType != USB_DT_ENDPOINT))
		return -EINVAL;

	if (!nudc->driver || (nudc->gadget.speed == USB_SPEED_UNKNOWN))
		return -ESHUTDOWN;

	max = usb_endpoint_maxp(desc);

	spin_lock_irqsave (&nudc->lock, flags);
	uep->maxpacket = max & 0x7ff;

	ep->ep.desc = desc;
	ep->endpoint_address = desc->bEndpointAddress;

	if (ep->index != 0) {
		writel(max, epreg(ep, REG_EP_MPS));
		ep->ep.maxpacket = max;

		sram_addr = nuc980_udc_get_sram_base(nudc, max);

		if (sram_addr < 0) {
			spin_unlock_irqrestore(&nudc->lock, flags);
			return sram_addr;
		}

		writel(sram_addr, epreg(ep, REG_EP_BUFSTART));
		sram_addr = sram_addr + max;
		writel(sram_addr - 1, epreg(ep, REG_EP_BUFEND));
	}

	if (ep->index != 0) {
		ep->ep_num = desc->bEndpointAddress & ~USB_DIR_IN;
		ep->ep_dir = desc->bEndpointAddress & 0x80 ? 1 : 0;
		ep->ep_type = ep->ep.desc->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK;
		if (ep->ep_type == USB_ENDPOINT_XFER_ISOC) {
			ep->ep_type = EP_CFG_EPTYPE_ISO;
			ep->ep_mode = EP_RSPCTL_MODE_AUTO;
		} else if (ep->ep_type == USB_ENDPOINT_XFER_BULK) {
			ep->ep_type = EP_CFG_EPTYPE_BULK;
			ep->ep_mode = EP_RSPCTL_MODE_AUTO;
		} else if (ep->ep_type == USB_ENDPOINT_XFER_INT) {
			ep->ep_type = EP_CFG_EPTYPE_INT;
			ep->ep_mode = EP_RSPCTL_MODE_MANUAL;
		}
		writel(EP_RSPCTL_FLUSH | EP_RSPCTL_TOGGLE,
				epreg(ep, REG_EP_RSPCTL));
		writel(ep->ep_num << 4 | ep->ep_dir << 3 | ep->ep_type | EP_CFG_EPEN,
				epreg(ep, REG_EP_CFG));
		writel(ep->ep_mode, epreg(ep, REG_EP_RSPCTL));

		/* enable irqs */
		int_en_reg = readl(nudc->base + REG_GINTEN);
		writel(int_en_reg | (1 << (ep->index + 1)), nudc->base + REG_GINTEN);
		nudc->irq_enbl = readl(nudc->base + REG_GINTEN);

		if (ep->ep_type == EP_CFG_EPTYPE_BULK) {
			if (ep->ep_dir)
				ep->irq_enb = 0x40;
			else
				ep->irq_enb = 0x1010;
		} else if (ep->ep_type == EP_CFG_EPTYPE_INT) {
			if (ep->ep_dir)
				ep->irq_enb = 0x40;
			else
				ep->irq_enb = 0x10;
		} else if (ep->ep_type == EP_CFG_EPTYPE_ISO) {
			if (ep->ep_dir)
				ep->irq_enb = 0x40;
			else
				ep->irq_enb = 0x20;
		}
	}
	spin_unlock_irqrestore(&nudc->lock, flags);

	return 0;
}

static int nuc980_udc_ep_disable(struct usb_ep *uep)
{
	struct nuc980_ep *ep;
	struct nuc980_udc *nudc;
	unsigned long flags;

	if (!uep)
		return -EINVAL;

	ep = container_of(uep, struct nuc980_ep, ep);

	if (!ep->ep.desc)
		return -EINVAL;
	nudc = ep->nudc;

	spin_lock_irqsave(&nudc->lock, flags);

	ep->ep.desc = NULL;

	writel(0, epreg(ep, REG_EP_CFG));
	writel(0, epreg(ep, REG_EP_INTEN));

	nuc980_udc_request_clear(nudc, ep, -ESHUTDOWN);

	writel(0, epreg(ep, REG_EP_BUFSTART));
	writel(0, epreg(ep, REG_EP_BUFEND));

	spin_unlock_irqrestore(&nudc->lock, flags);

	return 0;
}

static struct usb_request *nuc980_udc_alloc_request(struct usb_ep *uep,
						gfp_t mem_flags)
{
	struct nuc980_ep *ep;
	struct nuc980_udc *nudc;
	struct nuc980_request *req;

	if (!uep)
		return NULL;

	ep = container_of(uep, struct nuc980_ep, ep);
	nudc = ep->nudc;

	req = kzalloc(sizeof(*req), mem_flags);
	if (!req)
		return NULL;
	INIT_LIST_HEAD(&req->queue);

	return &req->req;
}

static void nuc980_udc_free_request(struct usb_ep *uep,
						struct usb_request *ureq)
{
	struct nuc980_request *req;
	struct nuc980_ep *ep;
	struct nuc980_udc *nudc;

	if (!uep)
		return;

	ep = container_of(uep, struct nuc980_ep, ep);
	nudc = ep->nudc;

	if (!ureq || (!ep->ep.desc && (ep->ep.name != nudc->ep[0].name)))
		return;

	req = container_of(ureq, struct nuc980_request, req);
	list_del_init(&req->queue);

	WARN_ON(!list_empty (&req->queue));
	kfree(req);
}

static int nuc980_udc_queue(struct usb_ep *uep, struct usb_request *ureq,
						gfp_t gfp_flags)
{
	struct nuc980_request *req;
	struct nuc980_ep *ep;
	struct nuc980_udc *nudc;
	unsigned long flags;

	if (!uep)
		return -EINVAL;

	ep = container_of(uep, struct nuc980_ep, ep);
	nudc = ep->nudc;

	if (!ep->ep.desc && (ep->ep.name != nudc->ep[0].name))
		return -EINVAL;

	if (!nudc->driver || (nudc->gadget.speed == USB_SPEED_UNKNOWN))
		return -ESHUTDOWN;

	local_irq_save(flags);

	req = container_of(ureq, struct nuc980_request, req);

	if (!ureq->complete || !ureq->buf || !list_empty(&req->queue)) {
		local_irq_restore(flags);
		return -EINVAL;
	}

	if (ep->ep.desc) {
		if ((ep->ep.desc->bmAttributes == USB_ENDPOINT_XFER_ISOC) &&
			(req->req.length > usb_endpoint_maxp(ep->ep.desc))) {
			local_irq_restore(flags);
			return -EMSGSIZE;
		}
	}

	ureq->status = -EINPROGRESS;
	ureq->actual = 0;

	if ((ep->ep_num == 0) && (ep->ep_dir))
		ureq->zero = 1;

	if (req)
		list_add_tail(&req->queue, &ep->queue);

	if (ep->index == 0) {
		if ((req->req.length != 0) && (nudc->ep0state == EP0_END_XFER)) {
			nudc->ep0state = EP0_IN_DATA_PHASE;
			writel(0x0a, nudc->base + REG_CEPINTEN);
		}
		if ((nudc->setup_ret > 1000) || ((req->req.length == 0) &&
				(nudc->ep0state == EP0_OUT_DATA_PHASE))) {
			writel(0, nudc->base + REG_CEPCTL);
			writel(0x402, nudc->base + REG_CEPINTEN);
			nuc980_udc_request_done(ep, req, 0);
			writel(CEPCTL_FLUSH, nudc->base + REG_CEPCTL);
		}
	} else if (ep->index > 0) {
		writel(ep->irq_enb, epreg(ep, REG_EP_INTEN));
	}

	local_irq_restore(flags);
	return 0;
}

static int nuc980_udc_dequeue(struct usb_ep *uep, struct usb_request *ureq)
{
	struct nuc980_request *req;
	struct nuc980_ep *ep;
	struct nuc980_udc *nudc;
	unsigned long flags;
	int ret = -EINVAL;

	if (!uep || !ureq)
		return -EINVAL;

	ep = container_of(uep, struct nuc980_ep, ep);
	nudc = ep->nudc;

	if (!nudc->driver)
		return -ESHUTDOWN;

	spin_lock_irqsave(&nudc->lock, flags);
	list_for_each_entry(req, &ep->queue, queue) {
		if (&req->req == ureq) {
			list_del_init(&req->queue);
			ureq->status = -ECONNRESET;
			ret = 0;
			break;
		}
	}
	spin_unlock_irqrestore(&nudc->lock, flags);

	if (ret == 0)
		nuc980_udc_request_done(ep, req, -ECONNRESET);

	return ret;
}

static int nuc980_udc_set_halt(struct usb_ep *uep, int value)
{
	struct nuc980_ep *ep;
	struct nuc980_udc *nudc;
	unsigned long flags;
	u32 val;

	if (!uep)
		return -EINVAL;

	ep = container_of(uep, struct nuc980_ep, ep);
	nudc = ep->nudc;

	if (ep->ep_type == EP_CFG_EPTYPE_ISO)
		return -EINVAL;

	spin_lock_irqsave(&nudc->lock, flags);

	if (value && ep->ep_dir && !list_empty(&ep->queue)) {
		spin_unlock_irqrestore(&nudc->lock, flags);
		return -EAGAIN;
	}

	if (value == 1) {
		if (ep->ep_num == 0) {
			writel(CEPCTL_STALLEN, nudc->base + REG_CEPCTL);
			nudc->ep0state = EP0_STALL;
		} else {
			val = readl(epreg(ep, REG_EP_RSPCTL));
			writel((val & 0xf7) | EP_RSPCTL_HALT,
						epreg(ep, REG_EP_RSPCTL));
		}
	} else {
		if (ep->ep_num != 0) {
			writel(EP_RSPCTL_TOGGLE, epreg(ep, REG_EP_RSPCTL));
		}
	}

	spin_unlock_irqrestore(&nudc->lock, flags);

	return 0;
}

static int nuc980_udc_fifo_status(struct usb_ep *uep)
{
	struct nuc980_ep *ep;
	struct nuc980_udc *nudc;
	u32 bytes = 0;

	if (!uep)
		return -ENODEV;

	ep = container_of(uep, struct nuc980_ep, ep);
	nudc = ep->nudc;

	if (ep->ep_num == 0) {
		bytes = readl(nudc->base + REG_CEPDATCNT) & 0xffff;
	} else {
		bytes = readl(epreg(ep, REG_EP_DATCNT));
	}

	return bytes;
}

static void nuc980_udc_fifo_flush(struct usb_ep *uep)
{
	struct nuc980_ep *ep;
	struct nuc980_udc *nudc;
	unsigned long flags;

	if (!uep)
		return;

	ep = container_of(uep, struct nuc980_ep, ep);
	nudc = ep->nudc;

	spin_lock_irqsave(&nudc->lock, flags);

	if (ep->ep_num == 0) {
		writel(CEPCTL_FLUSH, nudc->base + REG_CEPCTL);
	} else {
		writel(EP_RSPCTL_FLUSH, epreg(ep, REG_EP_RSPCTL));
	}

	spin_unlock_irqrestore(&nudc->lock, flags);
}

static const struct usb_gadget_ops nuc980_udc_ops = {
	.get_frame       = nuc980_udc_get_frame,
	.wakeup          = nuc980_udc_wakeup,
	.set_selfpowered = nuc980_udc_set_selfpowered,
	.pullup          = nuc980_udc_pullup,
	.udc_start       = nuc980_udc_start,
	.udc_stop        = nuc980_udc_stop,
};

static const struct usb_ep_ops nuc980_ep_ops = {
	.enable        = nuc980_udc_ep_enable,
	.disable       = nuc980_udc_ep_disable,
	.alloc_request = nuc980_udc_alloc_request,
	.free_request  = nuc980_udc_free_request,
	.queue         = nuc980_udc_queue,
	.dequeue       = nuc980_udc_dequeue,
	.set_halt      = nuc980_udc_set_halt,
	.fifo_status   = nuc980_udc_fifo_status,
	.fifo_flush    = nuc980_udc_fifo_flush,
};

static int nuc980_udc_probe(struct platform_device *pdev)
{
	struct nuc980_udc *nudc;
	struct nuc980_ep *nep;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct reset_control *rst;
	struct resource res;
	int irq;
	int ret;
	int i;

	nudc = devm_kzalloc(dev, sizeof(*nudc), GFP_KERNEL);
	if (!nudc) {
		dev_err(dev, "unable to allocate memory\n");
		return -ENOMEM;
	}

	nudc->dev = dev;
	platform_set_drvdata(pdev, nudc);

	ret = of_address_to_resource(np, 0, &res);
	if (ret) {
		dev_err(dev, "unable to get mem region\n");
		return ret;
	}

	if (!res.start || resource_size(&res) < NUC980_UDC_REG_SIZE) {
		dev_err(dev, "mem region out of range\n");
		return -EINVAL;
	}

	dev_set_name(dev, "%08x.nuc980-udc", (unsigned int)res.start);

	nudc->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(nudc->clk)) {
		dev_err(dev, "unable to get clock\n");
		return PTR_ERR(nudc->clk);
	}

	ret = clk_prepare_enable(nudc->clk);
	if (ret) {
		dev_err(dev, "unable to enable clock\n");
		return ret;
	}

	rst = devm_reset_control_get_optional_shared(dev, NULL);
	if (!IS_ERR(rst))
		reset_control_deassert(rst);

	nudc->base = devm_ioremap_resource(dev, &res);
	if (IS_ERR(nudc->base)) {
		dev_err(dev, "unable to map mem region\n");
		ret = PTR_ERR(nudc->base);
		goto disable_clk;
	}

	irq = irq_of_parse_and_map(np, 0);
	if (irq <= 0) {
		dev_err(dev, "unable to get irq\n");
		ret = -EINVAL;
		goto disable_clk;
	}

	writel(readl(nudc->base + REG_PHYCTL) | PHYCTL_PHYEN,
					nudc->base + REG_PHYCTL);
	for (i = 0; i < USBD_TIMEOUT; i++) {
		writel(0x20, nudc->base + REG_EP_MPS);
		if (readl(nudc->base + REG_EP_MPS) == 0x20)
			break;
	}
	if (i == USBD_TIMEOUT) {
		dev_err(dev, "USB PHY init timeout\n");
		ret = -ETIMEDOUT;
		goto disable_clk;
	}

	nudc->sram_data[0][0]       = 0;
	nudc->sram_data[0][1]       = 0x40;
	nudc->usb_address           = 0;
	spin_lock_init(&nudc->lock);

	nudc->gadget.dev.parent     = dev;
	nudc->gadget.ops            = &nuc980_udc_ops;
	nudc->gadget.speed          = USB_SPEED_UNKNOWN;
	nudc->gadget.max_speed      = USB_SPEED_HIGH;
	nudc->ep0state              = EP0_IDLE;
	nudc->gadget.name           = dev_name(dev);

	writel(GINTEN_USBIEN | GINTEN_CEPIEN, nudc->base + REG_GINTEN);
	writel((BUSINTEN_RESUMEIEN | BUSINTEN_RSTIEN | BUSINTEN_VBUSDETIEN), 
				nudc->base + REG_BUSINTEN);
	writel(0, nudc->base + REG_FADDR);
	writel((CEPINTEN_SETUPPKIEN | CEPINTEN_STSDONEIEN),
				nudc->base + REG_CEPINTEN);

	INIT_LIST_HEAD(&nudc->gadget.ep_list);

	for (i = 0; i < NUM_ENDPOINTS; i++) {
		nep = &nudc->ep[i];
		nep->index = i;
		nep->nudc = nudc;
		nep->ep_dir = 0xff;
		nep->ep_type = 0xff;
		snprintf(nep->name, EP_NAME_SIZE, "ep%d", i);
		nep->ep.name = nep->name;
		nep->ep.ops = &nuc980_ep_ops;
		list_add_tail(&nep->ep.ep_list, &nudc->gadget.ep_list);
		memset(&nep->ep.caps, 0, sizeof(nep->ep.caps));
		nep->ep.caps.dir_in = 1;
		nep->ep.caps.dir_out = 1;
		if (!i) {
			nep->ep_num = 0;
			nep->ep.caps.type_control = 1;
			nep->ep.maxpacket = EP0_FIFO_SIZE;
			usb_ep_set_maxpacket_limit(&nep->ep, EP0_FIFO_SIZE);
			writel(0x00, nudc->base + REG_CEPBUFSTART);
			writel(0x3f, nudc->base + REG_CEPBUFEND);
		} else {
			nep->ep_num = 0xff;
			nep->ep.caps.type_iso = 1;
			nep->ep.caps.type_bulk = 1;
			nep->ep.caps.type_int = 1;
			nep->ep.maxpacket = EP_FIFO_SIZE;
			usb_ep_set_maxpacket_limit(&nep->ep, EP_FIFO_SIZE);
			writel(0, nepreg(nudc, REG_EP_BUFSTART, i));
			writel(0, nepreg(nudc, REG_EP_BUFEND, i));
		}
		nep->ep.desc = NULL;
		INIT_LIST_HEAD(&nep->queue);
	}

	nudc->gadget.ep0 = &nudc->ep[0].ep;
	list_del_init(&nudc->ep[0].ep.ep_list);
	nudc->dma_vaddr = dma_alloc_wc(dev, 512,
					&nudc->dma_paddr, GFP_KERNEL);

	ret = devm_request_irq(dev, irq, nuc980_udc_irq, 0,
					dev_name(dev), nudc);

	if (ret) {
		dev_err(dev, "unable to request irq\n");
		goto free_dma;
	}

	ret = usb_add_gadget_udc(dev, &nudc->gadget);
	if (ret) {
		dev_err(dev, "unable to register udc gadget\n");
		goto free_dma;
	}

	dev_info(dev, "initialized\n");

	return 0;

free_dma:
	dma_free_wc(dev, 512, nudc->dma_vaddr, nudc->dma_paddr);
disable_clk:
	clk_disable_unprepare(nudc->clk);
	return ret;
}

static const struct of_device_id nuc980_udc_dt_ids[] = {
	{ .compatible = "nuvoton,nuc980-udc" },
	{},
};
MODULE_DEVICE_TABLE(of, nuc980_udc_dt_ids);

static struct platform_driver nuc980_udc_driver = {
	.probe = nuc980_udc_probe,
	.driver = {
		.name = "nuc980-udc",
		.of_match_table = nuc980_udc_dt_ids,
	},
};

static int __init nuc980_udc_init(void)
{
	return platform_driver_register(&nuc980_udc_driver);
}
device_initcall(nuc980_udc_init);
