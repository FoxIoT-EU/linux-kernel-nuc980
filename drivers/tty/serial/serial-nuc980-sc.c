// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022, Veiko Rütter <rebane@alkohol.ee>
 *
 * Based on previous work from:
 * Copyright (C) 2018 Nuvoton Technology Corp.
 *
 */

#include <linux/clk.h>
#include <linux/console.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/serial.h>
#include <linux/serial_core.h>
#include <linux/serial_reg.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>

#define NUC980_SCUART_MAJOR    204
#define NUC980_SCUART_MINOR    60

#define REG_DAT                0x000
#define REG_CTL                0x004
#define   CTL_NSB              BIT(15)
#define REG_ALTCTL             0x008
#define   ALTCTL_TXRST         BIT(0)
#define   ALTCTL_RXRST         BIT(1)
#define REG_RXTOUT             0x010
#define REG_ETUCTL             0x014
#define REG_INTEN              0x018
#define   INTEN_RDAIEN         BIT(0)
#define   INTEN_TBEIEN         BIT(1)
#define   INTEN_RXTOIEN        BIT(9)
#define REG_INTSTS             0x01C
#define   INTSTS_RDAIF         BIT(0)
#define   INTSTS_TBEIF         BIT(1)
#define   INTSTS_RXTOIF        BIT(9)
#define REG_STATUS             0x020
#define   STATUS_RXOV          BIT(0)
#define   STATUS_RXEMPTY       BIT(1)
#define   STATUS_PEF           BIT(4)
#define   STATUS_FEF           BIT(5)
#define   STATUS_BEF           BIT(6)
#define   STATUS_TXOV          BIT(8)
#define   STATUS_TXEMPTY       BIT(9)
#define   STATUS_TXFULL        BIT(10)
#define REG_UARTCTL            0x034
#define   UARTCTL_UARTEN       BIT(0)
#define   UARTCTL_PBOFF        BIT(6)
#define   UARTCTL_OPE          BIT(7)
#define NUC980_SCUART_REG_SIZE 0x100

#define TX_FIFO_SIZE           4

struct nuc980_serial_sc_port {
	void __iomem       *base;
	struct clk         *eclk;
	struct clk         *pclk;
	int                irq;
	struct device      *dev;
	struct device_node *np;
	struct uart_port   port;
	bool               initialized;
};

static int nuc980_serial_sc_count;
static struct nuc980_serial_sc_port *nuc980_serial_sc_ports;

static struct uart_driver nuc980_serial_sc_driver = {
	.owner       = THIS_MODULE,
	.driver_name = "nuc980-scuart",
	.dev_name    = "ttySC",
	.major       = NUC980_SCUART_MAJOR,
	.minor       = NUC980_SCUART_MINOR,
};

#define to_nport(_port) container_of(_port, struct nuc980_serial_sc_port, port)

static void nuc980_serial_sc_stop_tx(struct uart_port *port);
static void nuc980_serial_sc_of_enumerate(void);

static void nuc980_serial_sc_receive(struct nuc980_serial_sc_port *nport)
{
	struct uart_port *port = &nport->port;
	char tty_flag;
	u32 status;
	u32 c;
	int i;

	for (i = 0; i < 10000; i++) {
		status = readl(nport->base + REG_STATUS);
		if (status & STATUS_RXEMPTY)
			break;

		tty_flag = TTY_NORMAL;

		port->icount.rx++;

		if (unlikely(status & (STATUS_BEF | STATUS_FEF | STATUS_PEF | STATUS_RXOV))) {
			if (status & STATUS_BEF) {
				writel(STATUS_BEF, nport->base + REG_STATUS);
				port->icount.brk++;
				if (uart_handle_break(port))
					continue;
			}

			if (status & STATUS_PEF) {
				writel(STATUS_PEF, nport->base + REG_STATUS);
				port->icount.parity++;
			}

			if (status & STATUS_FEF) {
				writel(STATUS_FEF, nport->base + REG_STATUS);
				port->icount.frame++;
			}

			if (status & STATUS_RXOV) {
				writel(STATUS_RXOV, nport->base + REG_STATUS);
				port->icount.overrun++;
			}

			status &= port->read_status_mask;

			if (status & STATUS_BEF)
				tty_flag = TTY_BREAK;
			if (status & STATUS_PEF)
				tty_flag = TTY_PARITY;
			if (status & STATUS_FEF)
				tty_flag = TTY_FRAME;
		}

		c = readl(nport->base + REG_DAT);

		uart_insert_char(port, status, STATUS_RXOV, c, tty_flag);
	}
}

static void nuc980_serial_sc_transmit(struct nuc980_serial_sc_port *nport)
{
	struct uart_port *port = &nport->port;
	struct tty_port *tport = &port->state->port;
	unsigned char ch;
	int i;

	if (port->x_char) {
		if (readl(nport->base + REG_STATUS) & STATUS_TXFULL)
			return;

		writel(port->x_char, nport->base + REG_DAT);
		port->icount.tx++;
		port->x_char = 0;
		return;
	}

	if (uart_tx_stopped(port) || kfifo_is_empty(&tport->xmit_fifo)) {
		nuc980_serial_sc_stop_tx(port);
		return;
	}

	for (i = 0; i < TX_FIFO_SIZE; i++) {
		if (readl(nport->base + REG_STATUS) & STATUS_TXFULL)
			break;

		if (!kfifo_get(&tport->xmit_fifo, &ch))
			break;

		writel(ch, nport->base + REG_DAT);
		port->icount.tx++;
	}

	if (kfifo_len(&tport->xmit_fifo) < WAKEUP_CHARS)
		uart_write_wakeup(port);

	if (kfifo_is_empty(&tport->xmit_fifo))
		nuc980_serial_sc_stop_tx(port);
}

static irqreturn_t nuc980_serial_sc_irq(int irq, void *dev_id)
{
	struct nuc980_serial_sc_port *nport = dev_id;
	struct uart_port *port = &nport->port;
	unsigned long flags;
	bool rx_done = false;
	u32 intsts;
	u32 status;

	spin_lock_irqsave(&port->lock, flags);

	intsts = readl(nport->base + REG_INTSTS);
	status = readl(nport->base + REG_STATUS);

	if (intsts & (INTSTS_RXTOIF | INTSTS_RDAIF)) {
		if (port->ignore_status_mask & STATUS_RXEMPTY) {
			writel(readl(nport->base + REG_INTEN) & ~(INTEN_RXTOIEN | INTEN_RDAIEN),
						nport->base + REG_INTEN);
		} else {
			nuc980_serial_sc_receive(nport);
			rx_done = true;
		}
	}

	if (intsts & INTSTS_TBEIF)
		nuc980_serial_sc_transmit(nport);

	if (status & (STATUS_BEF | STATUS_FEF | STATUS_PEF | STATUS_RXOV | STATUS_TXOV))
		writel((STATUS_BEF | STATUS_FEF | STATUS_PEF | STATUS_RXOV | STATUS_TXOV),
			nport->base + REG_STATUS);

	spin_unlock_irqrestore(&port->lock, flags);

	if (rx_done)
		tty_flip_buffer_push(&port->state->port);

	return IRQ_HANDLED;
}

static int nuc980_serial_sc_startup(struct uart_port *port)
{
	struct nuc980_serial_sc_port *nport = to_nport(port);
	struct device *dev = nport->dev;
	int ret;

	writel((ALTCTL_RXRST | ALTCTL_TXRST), nport->base + REG_ALTCTL);
	writel(0xffffffff, nport->base + REG_INTSTS);

	ret = devm_request_irq(dev, port->irq, nuc980_serial_sc_irq,
				port->irqflags, dev_name(dev), nport);

	if (ret) {
		dev_err(dev, "unable to request irq\n");
		return ret;
	}

	writel(0x41, nport->base + REG_CTL);
	writel(readl(nport->base + REG_UARTCTL) | UARTCTL_UARTEN,
						nport->base + REG_UARTCTL);
	writel(0x40, nport->base + REG_RXTOUT);
	writel(INTEN_RXTOIEN | INTEN_RDAIEN, nport->base + REG_INTEN);

	return 0;
}

static void nuc980_serial_sc_shutdown(struct uart_port *port)
{
	struct nuc980_serial_sc_port *nport = to_nport(port);
	struct device *dev = nport->dev;

	writel(0x00, nport->base + REG_INTEN);
	writel((ALTCTL_RXRST | ALTCTL_TXRST), nport->base + REG_ALTCTL);
	devm_free_irq(dev, nport->irq, nport);
}

static void nuc980_serial_sc_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
}

static unsigned int nuc980_serial_sc_get_mctrl(struct uart_port *port)
{
	return 0;
}

static void nuc980_serial_sc_set_termios(struct uart_port *port,
					struct ktermios *termios,
					const struct ktermios *old)
{
	struct nuc980_serial_sc_port *nport = to_nport(port);
	unsigned long flags;
	unsigned long rate;
	unsigned int baud;
	u32 uartctl = UARTCTL_UARTEN;
	u32 quot;
	u32 ctl;

	switch (termios->c_cflag & CSIZE) {
	case CS5:
		uartctl |= 0x30;
		break;
	case CS6:
		uartctl |= 0x20;
		break;
	case CS7:
		uartctl |= 0x10;
		break;
	default:
	case CS8:
		uartctl |= 0;
		break;
	}

	if (!(termios->c_cflag & PARENB))
		uartctl |= UARTCTL_PBOFF;

	if (termios->c_cflag & PARODD)
		uartctl |= UARTCTL_OPE;

	rate = clk_get_rate(nport->eclk);
	baud = uart_get_baud_rate(port, termios, old, rate / 0xfff, rate / 5);
	quot = ((rate + baud / 2) / baud) - 1;

	spin_lock_irqsave(&port->lock, flags);

	port->read_status_mask = STATUS_RXOV;
	if (termios->c_iflag & INPCK)
		port->read_status_mask |= STATUS_FEF | STATUS_PEF;
	if (termios->c_iflag & (BRKINT | PARMRK))
		port->read_status_mask |= STATUS_BEF;

	port->ignore_status_mask = 0;
	if (termios->c_iflag & IGNPAR)
		port->ignore_status_mask |= STATUS_FEF | STATUS_PEF;
	if (termios->c_iflag & IGNBRK) {
		port->ignore_status_mask |= STATUS_BEF;
		if (termios->c_iflag & IGNPAR)
			port->ignore_status_mask |= STATUS_RXOV;
	}

	ctl = readl(nport->base + REG_CTL);

	if (termios->c_cflag & CSTOPB)
		ctl &= ~CTL_NSB;
	else
		ctl |= CTL_NSB;

	writel(ctl, nport->base + REG_CTL);

	writel(quot, nport->base + REG_ETUCTL);
	writel(uartctl, nport->base + REG_UARTCTL);

	spin_unlock_irqrestore(&port->lock, flags);
}

static void nuc980_serial_sc_start_tx(struct uart_port *port)
{
	struct nuc980_serial_sc_port *nport = to_nport(port);
	u32 inten;

	inten = readl(nport->base + REG_INTEN);
	if (!(inten & INTEN_TBEIEN))
		writel(inten | INTEN_TBEIEN, nport->base + REG_INTEN);
}

static void nuc980_serial_sc_stop_tx(struct uart_port *port)
{
	struct nuc980_serial_sc_port *nport = to_nport(port);
	u32 inten;

	inten = readl(nport->base + REG_INTEN);
	if (inten & INTEN_TBEIEN)
		writel(inten & ~INTEN_TBEIEN, nport->base + REG_INTEN);
}

static void nuc980_serial_sc_stop_rx(struct uart_port *port)
{
	struct nuc980_serial_sc_port *nport = to_nport(port);
	u32 inten;

	inten = readl(nport->base + REG_INTEN);
	writel(inten & ~(INTEN_RXTOIEN | INTEN_RDAIEN), nport->base + REG_INTEN);
}

static unsigned int nuc980_serial_sc_tx_empty(struct uart_port *port)
{
	struct nuc980_serial_sc_port *nport = to_nport(port);

	if (readl(nport->base + REG_STATUS) & STATUS_TXEMPTY)
		return TIOCSER_TEMT;

	return 0;
}

static void nuc980_serial_sc_enable_ms(struct uart_port *port)
{
}

static void nuc980_serial_sc_break_ctl(struct uart_port *port, int break_state)
{
}

static const char *nuc980_serial_sc_type(struct uart_port *port)
{
	return (port->type == PORT_NUC980_SC) ? "nuc980-scuart" : NULL;
}

static void nuc980_serial_sc_set_ldisc(struct uart_port *port,
						struct ktermios *termios)
{
}

static void nuc980_serial_sc_config_port(struct uart_port *port, int flags)
{
}

static void nuc980_serial_sc_release_port(struct uart_port *port)
{
}

static int nuc980_serial_sc_request_port(struct uart_port *port)
{
	return 0;
}

static struct uart_ops nuc980_serial_sc_ops = {
	.startup	= nuc980_serial_sc_startup,
	.shutdown	= nuc980_serial_sc_shutdown,
	.set_mctrl	= nuc980_serial_sc_set_mctrl,
	.get_mctrl	= nuc980_serial_sc_get_mctrl,
	.set_termios	= nuc980_serial_sc_set_termios,
	.start_tx	= nuc980_serial_sc_start_tx,
	.stop_tx	= nuc980_serial_sc_stop_tx,
	.stop_rx	= nuc980_serial_sc_stop_rx,
	.tx_empty	= nuc980_serial_sc_tx_empty,
	.enable_ms	= nuc980_serial_sc_enable_ms,
	.break_ctl	= nuc980_serial_sc_break_ctl,
	.type		= nuc980_serial_sc_type,
	.set_ldisc	= nuc980_serial_sc_set_ldisc,
	.config_port	= nuc980_serial_sc_config_port,
	.request_port	= nuc980_serial_sc_request_port,
	.release_port	= nuc980_serial_sc_release_port,
};

static int nuc980_serial_sc_probe(struct platform_device *pdev)
{
	struct nuc980_serial_sc_port *nport;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct uart_port *port;
	struct reset_control *rst;
	struct resource res;
	int idx;
	int ret;

	for (idx = 0; idx < nuc980_serial_sc_count; idx++) {
		nport = &nuc980_serial_sc_ports[idx];
		if (nport->np == np)
			break;
	}

	if (idx >= nuc980_serial_sc_count)
		return -EINVAL;

	if (!nport->np)
		return -EINVAL;

	platform_set_drvdata(pdev, nport);
	nport->dev = dev;
	port = &nport->port;

	ret = of_address_to_resource(nport->np, 0, &res);
	if (ret) {
		dev_err(dev, "unable to get mem region\n");
		return ret;
	}

	if (!res.start || resource_size(&res) < NUC980_SCUART_REG_SIZE) {
		dev_err(dev, "mem region out of range\n");
		return -EINVAL;
	}

	dev_set_name(dev, "%08x.nuc980-scuart", (unsigned int)res.start);

	nport->base = devm_ioremap_resource(dev, &res);
	if (IS_ERR(nport->base)) {
		dev_err(dev, "unable to map mem region\n");
                return PTR_ERR(nport->base);
	}

	nport->eclk = devm_clk_get(dev, "eclk");
	if (IS_ERR(nport->eclk)) {
		dev_err(dev, "unable to get clock: eclk\n");
		return PTR_ERR(nport->eclk);
	}

	nport->pclk = devm_clk_get(dev, "pclk");
	if (IS_ERR(nport->pclk)) {
		dev_warn(dev, "unable to get clock: pclk\n");
		nport->pclk = NULL;
	}

	ret = clk_prepare_enable(nport->eclk);
	if (ret) {
		dev_err(dev, "unable to enable clock: eclk\n");
		return ret;
	}

	ret = clk_prepare_enable(nport->pclk);
	if (ret) {
		dev_err(dev, "unable to enable clock: pclk\n");
		goto disable_eclk;
	}

	rst = devm_reset_control_get_optional_shared(dev, NULL);
	if (!IS_ERR(rst))
		reset_control_deassert(rst);

	nport->irq = irq_of_parse_and_map(np, 0);

	if (nport->irq <= 0) {
		dev_err(dev, "unable to get irq\n");
		ret = -EINVAL;
		goto disable_pclk;
	}

	port->dev          = dev;
	port->iotype       = UPIO_MEM32;
	port->type         = PORT_NUC980_SC;
	port->fifosize     = TX_FIFO_SIZE;
	port->flags        = UPF_SKIP_TEST | UPF_FIXED_TYPE | UPF_IOREMAP;
	port->ops          = &nuc980_serial_sc_ops;
	port->membase      = nport->base;
	port->mapbase      = res.start;
	port->mapsize      = resource_size(&res);
	port->irq          = nport->irq;
	port->irqflags     = irq_get_trigger_type(nport->irq);

	nport->initialized = true;

	ret = uart_add_one_port(&nuc980_serial_sc_driver, port);
	if (ret) {
		dev_err(dev, "unable to add port\n");
		nport->initialized = false;
		goto disable_pclk;
	}

	// dev_info(dev, "initialized\n");
	return 0;

disable_pclk:
	clk_disable_unprepare(nport->pclk);
disable_eclk:
	clk_disable_unprepare(nport->eclk);
	return ret;
}

static const struct of_device_id __maybe_unused nuc980_serial_sc_dt_ids[] = {
	{ .compatible = "nuvoton,nuc980-scuart", },
	{ }
};

static void nuc980_serial_sc_of_enumerate(void)
{
	/* TODO: does not sort correctly if some serial aliases are pointing
		to the another driver */
	static bool enum_done = false;
	struct device_node *np;
	int alias;
	int count;
	int match;
	int i;

	if (enum_done)
		return;

	nuc980_serial_sc_count = 0;
	nuc980_serial_sc_driver.nr = 0;
	for_each_matching_node(np, nuc980_serial_sc_dt_ids) {
		nuc980_serial_sc_count++;
		alias = of_alias_get_id(np, "serial");
		if (alias >= 0) {
			if (nuc980_serial_sc_driver.nr < (alias + 1))
				nuc980_serial_sc_driver.nr = (alias + 1);
		}
	}

	nuc980_serial_sc_ports = kcalloc(nuc980_serial_sc_count,
					sizeof(*nuc980_serial_sc_ports),
					GFP_KERNEL);

	if (!nuc980_serial_sc_ports) {
		nuc980_serial_sc_count = 0;
		return;
	}

	/* find aliases */
	count = 0;
	for_each_matching_node(np, nuc980_serial_sc_dt_ids) {
		alias = of_alias_get_id(np, "serial");
		if (alias < 0)
			continue;

		nuc980_serial_sc_ports[count].np = np;
		nuc980_serial_sc_ports[count].port.line = alias;
		of_node_get(np);
		count++;
	}

	/* map others */
	match = 0;
	for_each_matching_node(np, nuc980_serial_sc_dt_ids) {
		alias = of_alias_get_id(np, "serial");
		if (alias >= 0)
			continue;

		do {
			for (i = 0; i < count; i++) {
				if (match == nuc980_serial_sc_ports[i].port.line) {
					match++;
					break;
				}
			}
		} while (i < count);
		nuc980_serial_sc_ports[count].np = np;
		nuc980_serial_sc_ports[count].port.line = match;
		of_node_get(np);
		count++;
	}

	enum_done = true;
}

static struct platform_driver nuc980_serial_sc_platform_driver = {
	.probe	= nuc980_serial_sc_probe,
	.driver = {
		.name		= "nuc980-scuart",
		.of_match_table	= nuc980_serial_sc_dt_ids,
	},
};

static int __init nuc980_serial_sc_init(void)
{
	int ret;

	nuc980_serial_sc_of_enumerate();
	if (!nuc980_serial_sc_count)
		return 0;

	ret = uart_register_driver(&nuc980_serial_sc_driver);
	if (unlikely(ret))
		return ret;

	ret = platform_driver_register(&nuc980_serial_sc_platform_driver);
	if (unlikely(ret))
		uart_unregister_driver(&nuc980_serial_sc_driver);

	return ret;
}
device_initcall(nuc980_serial_sc_init);
