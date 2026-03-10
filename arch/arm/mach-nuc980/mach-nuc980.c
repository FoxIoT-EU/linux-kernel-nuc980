// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020, Veiko Rütter <rebane@alkohol.ee>
 */

#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <linux/of_platform.h>

static struct map_desc nuc980_mach_io_desc[] __initdata = {
	/* SoC MMIO registers */
	[0] = {
		.virtual = 0xf0000000,
		.pfn     = __phys_to_pfn(0xb0000000),
		.length  = 0x00100000,
		.type    = MT_DEVICE
	},
};

static void __init nuc980_mach_map_io(void)
{
	u32 val;
	int i;

	iotable_init(nuc980_mach_io_desc, ARRAY_SIZE(nuc980_mach_io_desc));

	// unlock sys registers
	for (i = 0; i < 16; i++) {
		writel(0x59, (void __iomem *)0xf00001fc);
		writel(0x16, (void __iomem *)0xf00001fc);
		writel(0x88, (void __iomem *)0xf00001fc);
		val = readl((void __iomem *)0xf00001fc);
		if (val == 0x01)
			break;
	}

	// deassert CHIP reset
	writel(readl((void __iomem *)0xf0000060) & ~(1 << 0), (void __iomem *)0xf0000060);
	// deassert CPU reset
	writel(readl((void __iomem *)0xf0000060) & ~(1 << 2), (void __iomem *)0xf0000060);
	// deassert SDRAM reset
	writel(readl((void __iomem *)0xf0000060) & ~(1 << 6), (void __iomem *)0xf0000060);
	// deassert AIC reset
	writel(readl((void __iomem *)0xf0000064) & ~(1 << 4), (void __iomem *)0xf0000064);
	// deassert TIMER0 reset
	writel(readl((void __iomem *)0xf0000064) & ~(1 << 8), (void __iomem *)0xf0000064);
	// deassert TIMER1 reset
	writel(readl((void __iomem *)0xf0000064) & ~(1 << 9), (void __iomem *)0xf0000064);
	// deassert TIMER2 reset
	writel(readl((void __iomem *)0xf0000064) & ~(1 << 10), (void __iomem *)0xf0000064);
	// deassert TIMER3 reset
	writel(readl((void __iomem *)0xf0000064) & ~(1 << 11), (void __iomem *)0xf0000064);
	// deassert TIMER4 reset
	writel(readl((void __iomem *)0xf0000064) & ~(1 << 12), (void __iomem *)0xf0000064);
	// deassert TIMER5 reset
	writel(readl((void __iomem *)0xf0000064) & ~(1 << 13), (void __iomem *)0xf0000064);

	// force USB host mode
	writel(readl((void __iomem *)0xf0000030) | (1 << 11), (void __iomem *)0xf0000030);
	writel(readl((void __iomem *)0xf0000004) | (1 << 16), (void __iomem *)0xf0000004);
	// deassert USBH reset
	writel(readl((void __iomem *)0xf0000060) & ~(1 << 18), (void __iomem *)0xf0000060);
	// enable USBH clock
	writel(readl((void __iomem *)0xf0000210) | (1 << 18), (void __iomem *)0xf0000210);
	// enable USBH phy
	writel(0x0160, (volatile void __iomem *)0xf00150c4);
	writel(0x0520, (volatile void __iomem *)0xf00150c8);

	// mdio clock 600kHz
	writel(0xf9, (volatile void __iomem *)0xf0000240);
	// enable emac0 clock
	// writel(readl((void __iomem *)0xf0000210) | (1 << 16), (void __iomem *)0xf0000210);

	// lock sys registers
	writel(0x00, (void __iomem *)0xf00001fc);
}

static void __init nuc980_mach_init(void)
{
	pr_info("nuc980-mach: initialized\n");
}

static const char *nuc980_mach_dt_compat[] __initconst = {
	"nuvoton,nuc980",
	NULL,
};

DT_MACHINE_START(nuc980_mach_dt, "Nuvoton NUC980")
	.dt_compat    = nuc980_mach_dt_compat,
	.map_io       = nuc980_mach_map_io,
	.init_machine = nuc980_mach_init,
MACHINE_END
