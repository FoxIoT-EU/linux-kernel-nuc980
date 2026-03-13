// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020, Veiko Rütter <rebane@alkohol.ee>
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

#include "serial_mctrl_gpio.h"

#define NUC980_UART_MAJOR    204
#define NUC980_UART_MINOR    40

#define REG_RBR              0x000
#define REG_THR              0x000
#define REG_IER              0x004
#define   IER_RDA            BIT(0)
#define   IER_THRE           BIT(1)
#define   IER_RXTOIEN        BIT(4)
#define   IER_TOCNTEN        BIT(11)
#define   IER_ATO_RTS        BIT(12)
#define   IER_ATO_CTS        BIT(13)
#define REG_FCR              0x008
#define   FCR_RFR            BIT(1)
#define   FCR_TFR            BIT(2)
#define   FCR_RFITL_8        (0x2 << 4)
#define REG_LCR              0x00c
#define   LCR_WLS            0x00000003
#define   LCR_NSB            BIT(2)
#define   LCR_PBE            BIT(3)
#define   LCR_EPE            BIT(4)
#define   LCR_SPE            BIT(5)
#define   LCR_BCB            BIT(6)
#define REG_MCR              0x010
#define   MCR_RTS            BIT(1)
#define   MCR_RTS_POL        BIT(9)
#define REG_MSR              0x014
#define   MSR_CTS            BIT(4)
#define   MSR_CTS_POL        BIT(8)
#define REG_FSR              0x018
#define   FSR_RXO            BIT(0)
#define   FSR_PEF            BIT(4)
#define   FSR_FEF            BIT(5)
#define   FSR_BIF            BIT(6)
#define   FSR_RX_EMPTY       BIT(14)
#define   FSR_RX_FULL        BIT(15)
#define   FSR_TX_EMPTY       BIT(22)
#define   FSR_TX_FULL        BIT(23)
#define   FSR_TXO            BIT(24)
#define   FSR_TE             BIT(28)
#define REG_ISR              0x01c
#define   ISR_RDA            BIT(0)
#define   ISR_THRE           BIT(1)
#define   ISR_MODEM          BIT(2)
#define   ISR_RXTO           BIT(4)
#define REG_TOUT             0x020
#define REG_BAUD             0x024
#define REG_IRCR             0x028
#define REG_ALT_CSR          0x02c
#define   ALT_CSR_AUTO       BIT(10)
#define REG_FUN_SEL          0x030
#define   FUN_SEL_UART       0x00000000
#define   FUN_SEL_LIN        0x00000001
#define   FUN_SEL_IRDA       0x00000002
#define   FUN_SEL_RS485      0x00000003
#define   FUN_TXRX_DIS       BIT(7)
#define REG_LIN_CTL          0x034
#define REG_LIN_SR           0x038
#define REG_BRCOMP           0x03c
#define REG_WKCTL            0x040
#define REG_WKSTS            0x044
#define REG_DWKCOMP          0x048
#define NUC980_UART_REG_SIZE 0x100

#define TX_FIFO_SIZE         16

struct nuc980_serial_port {
	void __iomem       *base;
	struct clk         *eclk;
	struct clk         *pclk;
	int                irq;
	struct device      *dev;
	struct device_node *np;
	struct uart_port   port;
	struct mctrl_gpios *gpios;
	u32                mcr;
	bool               console;
	bool               initialized;
};

static int nuc980_serial_count;
static bool nuc980_serial_earlycon_enabled;
static struct console nuc980_serial_console;
static struct nuc980_serial_port *nuc980_serial_ports;
static struct nuc980_serial_port *nuc980_serial_console_port;

static struct uart_driver nuc980_serial_driver = {
	.owner       = THIS_MODULE,
	.driver_name = "nuc980-uart",
	.dev_name    = "ttyS",
	.major       = NUC980_UART_MAJOR,
	.minor       = NUC980_UART_MINOR,
	.cons        = &nuc980_serial_console,
};

#define to_nport(_port) container_of(_port, struct nuc980_serial_port, port)

static void nuc980_serial_stop_tx(struct uart_port *port);
static void nuc980_serial_of_enumerate(void);

static void nuc980_serial_receive(struct nuc980_serial_port *nport)
{
	struct uart_port *port = &nport->port;
	char tty_flag;
	u32 fsr;
	u32 c;
	int i;

	for (i = 0; i < 10000; i++) {
		fsr = readl(nport->base + REG_FSR);
		if (fsr & FSR_RX_EMPTY)
			break;

		tty_flag = TTY_NORMAL;

		port->icount.rx++;

		if (unlikely(fsr & (FSR_BIF | FSR_FEF | FSR_PEF | FSR_RXO))) {
			if (fsr & FSR_BIF) {
				writel(FSR_BIF, nport->base + REG_FSR);
				port->icount.brk++;
				if (uart_handle_break(port))
					continue;
			}

			if (fsr & FSR_PEF) {
				writel(FSR_PEF, nport->base + REG_FSR);
				port->icount.parity++;
			}

			if (fsr & FSR_FEF) {
				writel(FSR_FEF, nport->base + REG_FSR);
				port->icount.frame++;
			}

			if (fsr & FSR_RXO) {
				writel(FSR_RXO, nport->base + REG_FSR);
				port->icount.overrun++;
			}

			fsr &= port->read_status_mask;

			if (fsr & FSR_BIF)
				tty_flag = TTY_BREAK;
			if (fsr & FSR_PEF)
				tty_flag = TTY_PARITY;
			if (fsr & FSR_FEF)
				tty_flag = TTY_FRAME;
		}

		c = readl(nport->base + REG_RBR);

		uart_insert_char(port, fsr, FSR_RXO, c, tty_flag);
	}
}

static void nuc980_serial_transmit(struct nuc980_serial_port *nport)
{
	struct uart_port *port = &nport->port;
	struct tty_port *tport = &port->state->port;
	unsigned char ch;
	int i;

	if (port->x_char) {
		if (readl(nport->base + REG_FSR) & FSR_TX_FULL)
			return;

		writel(port->x_char, nport->base + REG_THR);
		port->icount.tx++;
		port->x_char = 0;
		return;
	}

	if (uart_tx_stopped(port) || kfifo_is_empty(&tport->xmit_fifo)) {
		nuc980_serial_stop_tx(port);
		return;
	}

	for (i = 0; i < TX_FIFO_SIZE; i++) {
		if (readl(nport->base + REG_FSR) & FSR_TX_FULL)
			break;

		if (!kfifo_get(&tport->xmit_fifo, &ch))
			break;

		writel(ch, nport->base + REG_THR);
		port->icount.tx++;
	}

	if (kfifo_len(&tport->xmit_fifo) < WAKEUP_CHARS)
		uart_write_wakeup(port);

	if (kfifo_is_empty(&tport->xmit_fifo))
		nuc980_serial_stop_tx(port);
}

static irqreturn_t nuc980_serial_irq(int irq, void *dev_id)
{
	struct nuc980_serial_port *nport = dev_id;
	struct uart_port *port = &nport->port;
	unsigned long flags;
	bool rx_done = false;
	u32 isr;
	u32 fsr;

	spin_lock_irqsave(&port->lock, flags);

	isr = readl(nport->base + REG_ISR);
	fsr = readl(nport->base + REG_FSR);

	if (isr & (ISR_RDA | ISR_RXTO)) {
		if (port->ignore_status_mask & FSR_RX_EMPTY) {
			writel(readl(nport->base + REG_IER) &
				~(IER_RDA | IER_RXTOIEN | IER_TOCNTEN),
						nport->base + REG_IER);
		} else {
			nuc980_serial_receive(nport);
			rx_done = true;
		}
	}

	if (isr & ISR_THRE)
		nuc980_serial_transmit(nport);

	if (fsr & (FSR_BIF | FSR_FEF | FSR_PEF | FSR_RXO | FSR_TXO))
		writel((FSR_BIF | FSR_FEF | FSR_PEF | FSR_RXO | FSR_TXO),
			nport->base + REG_FSR);

	spin_unlock_irqrestore(&port->lock, flags);

	if (rx_done)
		tty_flip_buffer_push(&port->state->port);

	return IRQ_HANDLED;
}

static int nuc980_serial_startup(struct uart_port *port)
{
	struct nuc980_serial_port *nport = to_nport(port);
	struct device *dev = nport->dev;
	int ret;

	if (!nport->console)
		writel(FCR_RFR | FCR_TFR | FCR_RFITL_8, nport->base + REG_FCR);

	writel(0xffffffff, nport->base + REG_ISR);
	writel(40, nport->base + REG_TOUT);

	ret = devm_request_irq(dev, nport->irq, nuc980_serial_irq,
				port->irqflags, dev_name(dev), nport);

	if (ret) {
		dev_err(dev, "unable to request irq\n");
		return ret;
	}

	writel(IER_RDA | IER_RXTOIEN | IER_TOCNTEN, nport->base + REG_IER);

	return 0;
}

static void nuc980_serial_shutdown(struct uart_port *port)
{
	struct nuc980_serial_port *nport = to_nport(port);
	struct device *dev = nport->dev;

	writel(0x00, nport->base + REG_IER);
	devm_free_irq(dev, nport->irq, nport);
	mctrl_gpio_disable_ms_sync(nport->gpios);
}

static void nuc980_serial_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	struct nuc980_serial_port *nport = to_nport(port);
	u32 mcr;

	if (readl(nport->base + REG_FUN_SEL) & 0x07)
		return;

	mctrl_gpio_set(nport->gpios, mctrl);

	mcr = readl(nport->base + REG_MCR);

	if (mctrl & TIOCM_RTS) {
		mcr |= MCR_RTS_POL;
		mcr &= ~MCR_RTS;
	} else {
		mcr |= MCR_RTS_POL;
		mcr |= MCR_RTS;
	}

	if (nport->mcr & UART_MCR_AFE) {
		mcr |= MCR_RTS_POL;
		mcr &= ~MCR_RTS;

		writel(readl(nport->base + REG_IER) |
			(IER_ATO_RTS | IER_ATO_CTS), nport->base + REG_IER);

		port->flags |= UPF_HARD_FLOW;
	} else {
		writel(readl(nport->base + REG_IER) &
			~(IER_ATO_RTS | IER_ATO_CTS), nport->base + REG_IER);

		port->flags &= ~UPF_HARD_FLOW;
	}

	writel(readl(nport->base + REG_MSR) | MSR_CTS_POL,
						nport->base + REG_MSR);
	writel(mcr, nport->base + REG_MCR);
}

static unsigned int nuc980_serial_get_mctrl(struct uart_port *port)
{
	struct nuc980_serial_port *nport = to_nport(port);
	unsigned int mctrl = 0;

	mctrl_gpio_get(nport->gpios, &mctrl);

	if (readl(nport->base + REG_MSR) & MSR_CTS)
		mctrl |= TIOCM_CTS;
	else
		mctrl &= ~TIOCM_CTS;

	if (!mctrl_gpio_to_gpiod(nport->gpios, UART_GPIO_DSR))
		mctrl |= TIOCM_DSR;
	if (!mctrl_gpio_to_gpiod(nport->gpios, UART_GPIO_DCD))
		mctrl |= TIOCM_CAR;

	return mctrl;
}

static void nuc980_serial_set_termios(struct uart_port *port,
					struct ktermios *termios,
					const struct ktermios *old)
{
	struct nuc980_serial_port *nport = to_nport(port);
	unsigned long flags;
	unsigned long rate;
	unsigned int baud;
	u32 quot;
	u32 lcr;

	switch (termios->c_cflag & CSIZE) {
	case CS5:
		lcr = 0;
		break;
	case CS6:
		lcr = 1;
		break;
	case CS7:
		lcr = 2;
		break;
	default:
	case CS8:
		lcr = 3;
	}

	if (termios->c_cflag & CSTOPB)
		lcr |= LCR_NSB;
	if (termios->c_cflag & PARENB)
		lcr |= LCR_PBE;
	if (termios->c_cflag & CMSPAR) {
		lcr |= LCR_SPE;
		if (termios->c_cflag & PARODD)
			lcr |= LCR_EPE;
	} else {
		if (!(termios->c_cflag & PARODD))
			lcr |= LCR_EPE;
	}

	rate = clk_get_rate(nport->eclk);
	baud = uart_get_baud_rate(port, termios, old, rate / 65535, rate / 11);
	quot = (rate / baud) - 2;

	uart_update_timeout(port, termios->c_cflag, baud);

	spin_lock_irqsave(&port->lock, flags);

	port->read_status_mask = FSR_RXO;
	if (termios->c_iflag & INPCK)
		port->read_status_mask |= FSR_FEF | FSR_PEF;
	if (termios->c_iflag & (BRKINT | PARMRK))
		port->read_status_mask |= FSR_BIF;

	port->ignore_status_mask = 0;
	if (termios->c_iflag & IGNPAR)
		port->ignore_status_mask |= FSR_FEF | FSR_PEF;
	if (termios->c_iflag & IGNBRK) {
		port->ignore_status_mask |= FSR_BIF;
		if (termios->c_iflag & IGNPAR)
			port->ignore_status_mask |= FSR_RXO;
	}

	if (!(termios->c_cflag & CREAD)) {
		port->ignore_status_mask |= FSR_RX_EMPTY;
		writel(readl(nport->base + REG_IER) &
			~(IER_RDA | IER_RXTOIEN | IER_TOCNTEN),
						nport->base + REG_IER);
	} else {
		writel(readl(nport->base + REG_IER) |
			(IER_RDA | IER_RXTOIEN | IER_TOCNTEN),
						nport->base + REG_IER);
	}

	if (termios->c_cflag & CRTSCTS)
		nport->mcr |= UART_MCR_AFE;
	else
		nport->mcr &= ~UART_MCR_AFE;

	nuc980_serial_set_mctrl(port, port->mctrl);

	writel(quot | 0x30000000, nport->base + REG_BAUD);
	writel(lcr, nport->base + REG_LCR);

	spin_unlock_irqrestore(&port->lock, flags);
}

static void nuc980_serial_start_tx(struct uart_port *port)
{
	struct nuc980_serial_port *nport = to_nport(port);

	writel(readl(nport->base + REG_IER) | IER_THRE, nport->base + REG_IER);
}

static void nuc980_serial_stop_tx(struct uart_port *port)
{
	struct nuc980_serial_port *nport = to_nport(port);

	writel(readl(nport->base + REG_IER) & ~IER_THRE, nport->base + REG_IER);
}

static void nuc980_serial_stop_rx(struct uart_port *port)
{
	struct nuc980_serial_port *nport = to_nport(port);

	writel(readl(nport->base + REG_IER) &
			~(IER_RDA | IER_RXTOIEN | IER_TOCNTEN),
						nport->base + REG_IER);
}

static unsigned int nuc980_serial_tx_empty(struct uart_port *port)
{
	struct nuc980_serial_port *nport = to_nport(port);

	if (readl(nport->base + REG_FSR) & FSR_TX_EMPTY)
		return TIOCSER_TEMT;

	return 0;
}

static void nuc980_serial_enable_ms(struct uart_port *port)
{
	struct nuc980_serial_port *nport = to_nport(port);

	mctrl_gpio_enable_ms(nport->gpios);
}

static void nuc980_serial_break_ctl(struct uart_port *port, int break_state)
{
}

static const char *nuc980_serial_type(struct uart_port *port)
{
	return (port->type == PORT_NUC980) ? "nuc980-uart" : NULL;
}

static void nuc980_serial_set_ldisc(struct uart_port *port,
						struct ktermios *termios)
{
}

static void nuc980_serial_config_port(struct uart_port *port, int flags)
{
}

static void nuc980_serial_release_port(struct uart_port *port)
{
}

static int nuc980_serial_request_port(struct uart_port *port)
{
	return 0;
}

static int nuc980_serial_config_rs485(struct uart_port *port,
		struct ktermios *termios, struct serial_rs485 *rs485conf)
{
	// TODO: use variables and keep sync with nuc980_serial_get_mctrl ???
	struct nuc980_serial_port *nport = to_nport(port);
	u32 mcr;
	u32 val;

	val = readl(nport->base + REG_FUN_SEL) & ~0x07;

	if (rs485conf->flags & SER_RS485_ENABLED) {
		writel(val | FUN_SEL_RS485, nport->base + REG_FUN_SEL);
		mcr = readl(nport->base + REG_MCR) | MCR_RTS;

		if (rs485conf->flags & SER_RS485_RTS_ON_SEND) {
			writel(mcr & ~MCR_RTS_POL, nport->base + REG_MCR);
		} else {
			writel(mcr | MCR_RTS_POL, nport->base + REG_MCR);
		}

		writel(readl(nport->base + REG_ALT_CSR) | ALT_CSR_AUTO,
						nport->base + REG_ALT_CSR);
		writel(readl(nport->base + REG_IER) &
			~(IER_ATO_RTS | IER_ATO_CTS), nport->base + REG_IER);
	} else {
		writel(val, nport->base + REG_FUN_SEL);
	}

	return 0;
}

static const struct uart_ops nuc980_serial_ops = {
	.startup      = nuc980_serial_startup,
	.shutdown     = nuc980_serial_shutdown,
	.set_mctrl    = nuc980_serial_set_mctrl,
	.get_mctrl    = nuc980_serial_get_mctrl,
	.set_termios  = nuc980_serial_set_termios,
	.start_tx     = nuc980_serial_start_tx,
	.stop_tx      = nuc980_serial_stop_tx,
	.stop_rx      = nuc980_serial_stop_rx,
	.tx_empty     = nuc980_serial_tx_empty,
	.enable_ms    = nuc980_serial_enable_ms,
	.break_ctl    = nuc980_serial_break_ctl,
	.type         = nuc980_serial_type,
	.set_ldisc    = nuc980_serial_set_ldisc,
	.config_port  = nuc980_serial_config_port,
	.request_port = nuc980_serial_request_port,
	.release_port = nuc980_serial_release_port,
};

static void nuc980_serial_earlycon_putc(struct uart_port *port, unsigned char ch)
{
	if (!nuc980_serial_earlycon_enabled)
		return;

	while(readl(port->membase + REG_FSR) & FSR_TX_FULL)
		cpu_relax();

	writel(ch, port->membase + REG_THR);
}

static void nuc980_serial_earlycon_write(struct console *console,
				const char *s, unsigned n)
{
	struct earlycon_device *device = console->data;
	if (!nuc980_serial_earlycon_enabled)
		return;

        uart_console_write(&device->port, s, n, nuc980_serial_earlycon_putc);
}

static int __init nuc980_serial_earlycon_setup(struct earlycon_device *device,
					const char *options)
{
	if (!device->port.membase)
		return -ENODEV;

	device->con->write = nuc980_serial_earlycon_write;
	nuc980_serial_earlycon_enabled = true;
	return 0;
}
OF_EARLYCON_DECLARE(nuc980_earlycon, "nuvoton,nuc980-uart",
			nuc980_serial_earlycon_setup);

static void nuc980_serial_console_putc(struct uart_port *port, unsigned char  ch)
{
	struct nuc980_serial_port *nport = to_nport(port);

	while(readl(nport->base + REG_FSR) & FSR_TX_FULL)
		cpu_relax();

	writel(ch, nport->base + REG_THR);
}

static void nuc980_serial_console_write(struct console *console,
				const char *s, unsigned int n)
{
	if(!nuc980_serial_console_port || !nuc980_serial_console_port->console)
		return;

	uart_console_write(&nuc980_serial_console_port->port, s, n,
					nuc980_serial_console_putc);
}

static int __init nuc980_serial_console_setup(struct console *console,
						char *options)
{
	struct nuc980_serial_port *nport;
	struct device *dev;
	struct uart_port *port;
	int baud = 115200;
	int bits = 8;
	int parity = 'n';
	int flow = 'n';
	int idx;

	for (idx = 0; idx < nuc980_serial_count; idx++) {
		nport = &nuc980_serial_ports[idx];
		if (nport->port.line == console->index &&
					nport->initialized)
			break;
	}

	if (idx >= nuc980_serial_count)
		return -EINVAL;

	port = &nport->port;
	dev = nport->dev;

	nport->console = true;
	nuc980_serial_console_port = nport;

	if (of_stdout == nport->np)
		nuc980_serial_earlycon_enabled = false;

	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);

	uart_set_options(port, console, baud, parity, bits, flow);

	return 0;
}

static struct console nuc980_serial_console = {
	.name = "ttyS",
	.write = nuc980_serial_console_write,
	.device = uart_console_device,
	.setup = nuc980_serial_console_setup,
	.flags = CON_PRINTBUFFER,
	.index = -1,
	.data = &nuc980_serial_driver,
};

static int __init nuc980_serial_console_init(void)
{
	nuc980_serial_of_enumerate();
	register_console(&nuc980_serial_console);
	return 0;
}
console_initcall(nuc980_serial_console_init);

static const struct serial_rs485 nuc980_serial_rs485_supported = {
	.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND | SER_RS485_RTS_AFTER_SEND |
		 SER_RS485_RX_DURING_TX,
	.delay_rts_before_send = 1,
	.delay_rts_after_send = 1,
};

static int nuc980_serial_probe(struct platform_device *pdev)
{
	struct nuc980_serial_port *nport;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct uart_port *port;
	struct reset_control *rst;
	struct resource res;
	int idx;
	int ret;

	for (idx = 0; idx < nuc980_serial_count; idx++) {
		nport = &nuc980_serial_ports[idx];
		if (nport->np == np)
			break;
	}

	if (idx >= nuc980_serial_count)
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

	if (!res.start || resource_size(&res) < NUC980_UART_REG_SIZE) {
		dev_err(dev, "mem region out of range\n");
		return -EINVAL;
	}

	dev_set_name(dev, "%08x.nuc980-uart", (unsigned int)res.start);

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

	port->dev             = dev;
	port->iotype          = UPIO_MEM32;
	port->type            = PORT_NUC980;
	port->fifosize        = TX_FIFO_SIZE;
	port->flags           = UPF_SKIP_TEST | UPF_FIXED_TYPE | UPF_IOREMAP;
	port->ops             = &nuc980_serial_ops;
	port->membase         = nport->base;
	port->mapbase         = res.start;
	port->mapsize         = resource_size(&res);
	port->irq             = nport->irq;
	port->irqflags        = irq_get_trigger_type(nport->irq);
	port->rs485_config    = nuc980_serial_config_rs485;
	port->rs485_supported = nuc980_serial_rs485_supported;

	uart_get_rs485_mode(port);

	nport->gpios = mctrl_gpio_init(port, 0);
	if (IS_ERR(nport->gpios)) {
		dev_err(dev, "unable to get mctrl gpios\n");
		ret = PTR_ERR(nport->gpios);
		goto disable_pclk;
	}

	if (mctrl_gpio_to_gpiod(nport->gpios, UART_GPIO_CTS) ||
			mctrl_gpio_to_gpiod(nport->gpios, UART_GPIO_RTS)) {
		dev_err(dev, "conflicting RTS/CTS config\n");
		ret = -EINVAL;
		goto disable_pclk;
	}

	nport->initialized = true;

	ret = uart_add_one_port(&nuc980_serial_driver, port);
	if (ret) {
		dev_err(dev, "unable to add port\n");
		nport->initialized = false;
		goto disable_pclk;
	}

	if (nport->port.rs485.flags & SER_RS485_ENABLED)
					nuc980_serial_config_rs485(&nport->port,
					NULL, &nport->port.rs485);

	// dev_info(dev, "initialized\n");
	return 0;

disable_pclk:
	clk_disable_unprepare(nport->pclk);
disable_eclk:
	clk_disable_unprepare(nport->eclk);
	return ret;
}

static const struct of_device_id __maybe_unused nuc980_serial_dt_ids[] = {
	{ .compatible = "nuvoton,nuc980-uart", },
	{ }
};

static void nuc980_serial_of_enumerate(void)
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

	nuc980_serial_count = 0;
	nuc980_serial_driver.nr = 0;
	for_each_matching_node(np, nuc980_serial_dt_ids) {
		nuc980_serial_count++;
		alias = of_alias_get_id(np, "serial");
		if (alias >= 0) {
			if (nuc980_serial_driver.nr < (alias + 1))
				nuc980_serial_driver.nr = (alias + 1);
		}
	}

	nuc980_serial_ports = kcalloc(nuc980_serial_count,
					sizeof(*nuc980_serial_ports),
					GFP_KERNEL);

	if (!nuc980_serial_ports) {
		nuc980_serial_count = 0;
		return;
	}

	/* find aliases */
	count = 0;
	for_each_matching_node(np, nuc980_serial_dt_ids) {
		alias = of_alias_get_id(np, "serial");
		if (alias < 0)
			continue;

		nuc980_serial_ports[count].np = np;
		nuc980_serial_ports[count].port.line = alias;
		of_node_get(np);
		count++;
	}

	/* map others */
	match = 0;
	for_each_matching_node(np, nuc980_serial_dt_ids) {
		alias = of_alias_get_id(np, "serial");
		if (alias >= 0)
			continue;

		do {
			for (i = 0; i < count; i++) {
				if (match == nuc980_serial_ports[i].port.line) {
					match++;
					break;
				}
			}
		} while (i < count);
		nuc980_serial_ports[count].np = np;
		nuc980_serial_ports[count].port.line = match;
		of_node_get(np);
		count++;
	}

	enum_done = true;
}

static struct platform_driver nuc980_serial_platform_driver = {
	.probe	= nuc980_serial_probe,
	.driver = {
		.name		= "nuc980-uart",
		.of_match_table	= nuc980_serial_dt_ids,
	},
};

static int __init nuc980_serial_init(void)
{
	int ret;

	nuc980_serial_of_enumerate();
	if (!nuc980_serial_count)
		return 0;

	ret = uart_register_driver(&nuc980_serial_driver);
	if (unlikely(ret))
		return ret;

	ret = platform_driver_register(&nuc980_serial_platform_driver);
	if (unlikely(ret))
		uart_unregister_driver(&nuc980_serial_driver);

	return ret;
}
device_initcall(nuc980_serial_init);
