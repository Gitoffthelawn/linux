// SPDX-License-Identifier: GPL-2.0
/* sunhme.c: Sparc HME/BigMac 10/100baseT half/full duplex auto switching,
 *           auto carrier detecting ethernet driver.  Also known as the
 *           "Happy Meal Ethernet" found on SunSwift SBUS cards.
 *
 * Copyright (C) 1996, 1998, 1999, 2002, 2003,
 *		2006, 2008 David S. Miller (davem@davemloft.net)
 *
 * Changes :
 * 2000/11/11 Willy Tarreau <willy AT meta-x.org>
 *   - port to non-sparc architectures. Tested only on x86 and
 *     only currently works with QFE PCI cards.
 *   - ability to specify the MAC address at module load time by passing this
 *     argument : macaddr=0x00,0x10,0x20,0x30,0x40,0x50
 */

#include <linux/bitops.h>
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/fcntl.h>
#include <linux/in.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/mii.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/random.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include <asm/byteorder.h>
#include <asm/dma.h>
#include <asm/irq.h>

#ifdef CONFIG_SPARC
#include <asm/auxio.h>
#include <asm/idprom.h>
#include <asm/openprom.h>
#include <asm/oplib.h>
#include <asm/prom.h>
#endif

#include "sunhme.h"

#define DRV_NAME	"sunhme"

MODULE_AUTHOR("David S. Miller <davem@davemloft.net>");
MODULE_DESCRIPTION("Sun HappyMealEthernet(HME) 10/100baseT ethernet driver");
MODULE_LICENSE("GPL");

static int macaddr[6];

/* accept MAC address of the form macaddr=0x08,0x00,0x20,0x30,0x40,0x50 */
module_param_array(macaddr, int, NULL, 0);
MODULE_PARM_DESC(macaddr, "Happy Meal MAC address to set");

#ifdef CONFIG_SBUS
static struct quattro *qfe_sbus_list;
#endif

#ifdef CONFIG_PCI
static struct quattro *qfe_pci_list;
#endif

#define hme_debug(fmt, ...) pr_debug("%s: " fmt, __func__, ##__VA_ARGS__)
#define HMD hme_debug

/* "Auto Switch Debug" aka phy debug */
#if 1
#define ASD hme_debug
#else
#define ASD(...)
#endif

#if 0
struct hme_tx_logent {
	unsigned int tstamp;
	int tx_new, tx_old;
	unsigned int action;
#define TXLOG_ACTION_IRQ	0x01
#define TXLOG_ACTION_TXMIT	0x02
#define TXLOG_ACTION_TBUSY	0x04
#define TXLOG_ACTION_NBUFS	0x08
	unsigned int status;
};
#define TX_LOG_LEN	128
static struct hme_tx_logent tx_log[TX_LOG_LEN];
static int txlog_cur_entry;
static __inline__ void tx_add_log(struct happy_meal *hp, unsigned int a, unsigned int s)
{
	struct hme_tx_logent *tlp;
	unsigned long flags;

	local_irq_save(flags);
	tlp = &tx_log[txlog_cur_entry];
	tlp->tstamp = (unsigned int)jiffies;
	tlp->tx_new = hp->tx_new;
	tlp->tx_old = hp->tx_old;
	tlp->action = a;
	tlp->status = s;
	txlog_cur_entry = (txlog_cur_entry + 1) & (TX_LOG_LEN - 1);
	local_irq_restore(flags);
}
static __inline__ void tx_dump_log(void)
{
	int i, this;

	this = txlog_cur_entry;
	for (i = 0; i < TX_LOG_LEN; i++) {
		pr_err("TXLOG[%d]: j[%08x] tx[N(%d)O(%d)] action[%08x] stat[%08x]\n", i,
		       tx_log[this].tstamp,
		       tx_log[this].tx_new, tx_log[this].tx_old,
		       tx_log[this].action, tx_log[this].status);
		this = (this + 1) & (TX_LOG_LEN - 1);
	}
}
#else
#define tx_add_log(hp, a, s)
#define tx_dump_log()
#endif

#define DEFAULT_IPG0      16 /* For lance-mode only */
#define DEFAULT_IPG1       8 /* For all modes */
#define DEFAULT_IPG2       4 /* For all modes */
#define DEFAULT_JAMSIZE    4 /* Toe jam */

/* NOTE: In the descriptor writes one _must_ write the address
 *	 member _first_.  The card must not be allowed to see
 *	 the updated descriptor flags until the address is
 *	 correct.  I've added a write memory barrier between
 *	 the two stores so that I can sleep well at night... -DaveM
 */

#if defined(CONFIG_SBUS) && defined(CONFIG_PCI)
static void sbus_hme_write32(void __iomem *reg, u32 val)
{
	sbus_writel(val, reg);
}

static u32 sbus_hme_read32(void __iomem *reg)
{
	return sbus_readl(reg);
}

static void sbus_hme_write_rxd(struct happy_meal_rxd *rxd, u32 flags, u32 addr)
{
	rxd->rx_addr = (__force hme32)addr;
	dma_wmb();
	rxd->rx_flags = (__force hme32)flags;
}

static void sbus_hme_write_txd(struct happy_meal_txd *txd, u32 flags, u32 addr)
{
	txd->tx_addr = (__force hme32)addr;
	dma_wmb();
	txd->tx_flags = (__force hme32)flags;
}

static u32 sbus_hme_read_desc32(hme32 *p)
{
	return (__force u32)*p;
}

static void pci_hme_write32(void __iomem *reg, u32 val)
{
	writel(val, reg);
}

static u32 pci_hme_read32(void __iomem *reg)
{
	return readl(reg);
}

static void pci_hme_write_rxd(struct happy_meal_rxd *rxd, u32 flags, u32 addr)
{
	rxd->rx_addr = (__force hme32)cpu_to_le32(addr);
	dma_wmb();
	rxd->rx_flags = (__force hme32)cpu_to_le32(flags);
}

static void pci_hme_write_txd(struct happy_meal_txd *txd, u32 flags, u32 addr)
{
	txd->tx_addr = (__force hme32)cpu_to_le32(addr);
	dma_wmb();
	txd->tx_flags = (__force hme32)cpu_to_le32(flags);
}

static u32 pci_hme_read_desc32(hme32 *p)
{
	return le32_to_cpup((__le32 *)p);
}

#define hme_write32(__hp, __reg, __val) \
	((__hp)->write32((__reg), (__val)))
#define hme_read32(__hp, __reg) \
	((__hp)->read32(__reg))
#define hme_write_rxd(__hp, __rxd, __flags, __addr) \
	((__hp)->write_rxd((__rxd), (__flags), (__addr)))
#define hme_write_txd(__hp, __txd, __flags, __addr) \
	((__hp)->write_txd((__txd), (__flags), (__addr)))
#define hme_read_desc32(__hp, __p) \
	((__hp)->read_desc32(__p))
#else
#ifdef CONFIG_SBUS
/* SBUS only compilation */
#define hme_write32(__hp, __reg, __val) \
	sbus_writel((__val), (__reg))
#define hme_read32(__hp, __reg) \
	sbus_readl(__reg)
#define hme_write_rxd(__hp, __rxd, __flags, __addr) \
do {	(__rxd)->rx_addr = (__force hme32)(u32)(__addr); \
	dma_wmb(); \
	(__rxd)->rx_flags = (__force hme32)(u32)(__flags); \
} while(0)
#define hme_write_txd(__hp, __txd, __flags, __addr) \
do {	(__txd)->tx_addr = (__force hme32)(u32)(__addr); \
	dma_wmb(); \
	(__txd)->tx_flags = (__force hme32)(u32)(__flags); \
} while(0)
#define hme_read_desc32(__hp, __p)	((__force u32)(hme32)*(__p))
#else
/* PCI only compilation */
#define hme_write32(__hp, __reg, __val) \
	writel((__val), (__reg))
#define hme_read32(__hp, __reg) \
	readl(__reg)
#define hme_write_rxd(__hp, __rxd, __flags, __addr) \
do {	(__rxd)->rx_addr = (__force hme32)cpu_to_le32(__addr); \
	dma_wmb(); \
	(__rxd)->rx_flags = (__force hme32)cpu_to_le32(__flags); \
} while(0)
#define hme_write_txd(__hp, __txd, __flags, __addr) \
do {	(__txd)->tx_addr = (__force hme32)cpu_to_le32(__addr); \
	dma_wmb(); \
	(__txd)->tx_flags = (__force hme32)cpu_to_le32(__flags); \
} while(0)
static inline u32 hme_read_desc32(struct happy_meal *hp, hme32 *p)
{
	return le32_to_cpup((__le32 *)p);
}
#endif
#endif


/* Oh yes, the MIF BitBang is mighty fun to program.  BitBucket is more like it. */
static void BB_PUT_BIT(struct happy_meal *hp, void __iomem *tregs, int bit)
{
	hme_write32(hp, tregs + TCVR_BBDATA, bit);
	hme_write32(hp, tregs + TCVR_BBCLOCK, 0);
	hme_write32(hp, tregs + TCVR_BBCLOCK, 1);
}

#if 0
static u32 BB_GET_BIT(struct happy_meal *hp, void __iomem *tregs, int internal)
{
	u32 ret;

	hme_write32(hp, tregs + TCVR_BBCLOCK, 0);
	hme_write32(hp, tregs + TCVR_BBCLOCK, 1);
	ret = hme_read32(hp, tregs + TCVR_CFG);
	if (internal)
		ret &= TCV_CFG_MDIO0;
	else
		ret &= TCV_CFG_MDIO1;

	return ret;
}
#endif

static u32 BB_GET_BIT2(struct happy_meal *hp, void __iomem *tregs, int internal)
{
	u32 retval;

	hme_write32(hp, tregs + TCVR_BBCLOCK, 0);
	udelay(1);
	retval = hme_read32(hp, tregs + TCVR_CFG);
	if (internal)
		retval &= TCV_CFG_MDIO0;
	else
		retval &= TCV_CFG_MDIO1;
	hme_write32(hp, tregs + TCVR_BBCLOCK, 1);

	return retval;
}

#define TCVR_FAILURE      0x80000000     /* Impossible MIF read value */

static int happy_meal_bb_read(struct happy_meal *hp,
			      void __iomem *tregs, int reg)
{
	u32 tmp;
	int retval = 0;
	int i;

	/* Enable the MIF BitBang outputs. */
	hme_write32(hp, tregs + TCVR_BBOENAB, 1);

	/* Force BitBang into the idle state. */
	for (i = 0; i < 32; i++)
		BB_PUT_BIT(hp, tregs, 1);

	/* Give it the read sequence. */
	BB_PUT_BIT(hp, tregs, 0);
	BB_PUT_BIT(hp, tregs, 1);
	BB_PUT_BIT(hp, tregs, 1);
	BB_PUT_BIT(hp, tregs, 0);

	/* Give it the PHY address. */
	tmp = hp->paddr & 0xff;
	for (i = 4; i >= 0; i--)
		BB_PUT_BIT(hp, tregs, ((tmp >> i) & 1));

	/* Tell it what register we want to read. */
	tmp = (reg & 0xff);
	for (i = 4; i >= 0; i--)
		BB_PUT_BIT(hp, tregs, ((tmp >> i) & 1));

	/* Close down the MIF BitBang outputs. */
	hme_write32(hp, tregs + TCVR_BBOENAB, 0);

	/* Now read in the value. */
	(void) BB_GET_BIT2(hp, tregs, (hp->tcvr_type == internal));
	for (i = 15; i >= 0; i--)
		retval |= BB_GET_BIT2(hp, tregs, (hp->tcvr_type == internal));
	(void) BB_GET_BIT2(hp, tregs, (hp->tcvr_type == internal));
	(void) BB_GET_BIT2(hp, tregs, (hp->tcvr_type == internal));
	(void) BB_GET_BIT2(hp, tregs, (hp->tcvr_type == internal));
	ASD("reg=%d value=%x\n", reg, retval);
	return retval;
}

static void happy_meal_bb_write(struct happy_meal *hp,
				void __iomem *tregs, int reg,
				unsigned short value)
{
	u32 tmp;
	int i;

	ASD("reg=%d value=%x\n", reg, value);

	/* Enable the MIF BitBang outputs. */
	hme_write32(hp, tregs + TCVR_BBOENAB, 1);

	/* Force BitBang into the idle state. */
	for (i = 0; i < 32; i++)
		BB_PUT_BIT(hp, tregs, 1);

	/* Give it write sequence. */
	BB_PUT_BIT(hp, tregs, 0);
	BB_PUT_BIT(hp, tregs, 1);
	BB_PUT_BIT(hp, tregs, 0);
	BB_PUT_BIT(hp, tregs, 1);

	/* Give it the PHY address. */
	tmp = (hp->paddr & 0xff);
	for (i = 4; i >= 0; i--)
		BB_PUT_BIT(hp, tregs, ((tmp >> i) & 1));

	/* Tell it what register we will be writing. */
	tmp = (reg & 0xff);
	for (i = 4; i >= 0; i--)
		BB_PUT_BIT(hp, tregs, ((tmp >> i) & 1));

	/* Tell it to become ready for the bits. */
	BB_PUT_BIT(hp, tregs, 1);
	BB_PUT_BIT(hp, tregs, 0);

	for (i = 15; i >= 0; i--)
		BB_PUT_BIT(hp, tregs, ((value >> i) & 1));

	/* Close down the MIF BitBang outputs. */
	hme_write32(hp, tregs + TCVR_BBOENAB, 0);
}

#define TCVR_READ_TRIES   16

static int happy_meal_tcvr_read(struct happy_meal *hp,
				void __iomem *tregs, int reg)
{
	int tries = TCVR_READ_TRIES;
	int retval;

	if (hp->tcvr_type == none) {
		ASD("no transceiver, value=TCVR_FAILURE\n");
		return TCVR_FAILURE;
	}

	if (!(hp->happy_flags & HFLAG_FENABLE)) {
		ASD("doing bit bang\n");
		return happy_meal_bb_read(hp, tregs, reg);
	}

	hme_write32(hp, tregs + TCVR_FRAME,
		    (FRAME_READ | (hp->paddr << 23) | ((reg & 0xff) << 18)));
	while (!(hme_read32(hp, tregs + TCVR_FRAME) & 0x10000) && --tries)
		udelay(20);
	if (!tries) {
		netdev_err(hp->dev, "Aieee, transceiver MIF read bolixed\n");
		return TCVR_FAILURE;
	}
	retval = hme_read32(hp, tregs + TCVR_FRAME) & 0xffff;
	ASD("reg=0x%02x value=%04x\n", reg, retval);
	return retval;
}

#define TCVR_WRITE_TRIES  16

static void happy_meal_tcvr_write(struct happy_meal *hp,
				  void __iomem *tregs, int reg,
				  unsigned short value)
{
	int tries = TCVR_WRITE_TRIES;

	ASD("reg=0x%02x value=%04x\n", reg, value);

	/* Welcome to Sun Microsystems, can I take your order please? */
	if (!(hp->happy_flags & HFLAG_FENABLE)) {
		happy_meal_bb_write(hp, tregs, reg, value);
		return;
	}

	/* Would you like fries with that? */
	hme_write32(hp, tregs + TCVR_FRAME,
		    (FRAME_WRITE | (hp->paddr << 23) |
		     ((reg & 0xff) << 18) | (value & 0xffff)));
	while (!(hme_read32(hp, tregs + TCVR_FRAME) & 0x10000) && --tries)
		udelay(20);

	/* Anything else? */
	if (!tries)
		netdev_err(hp->dev, "Aieee, transceiver MIF write bolixed\n");

	/* Fifty-two cents is your change, have a nice day. */
}

/* Auto negotiation.  The scheme is very simple.  We have a timer routine
 * that keeps watching the auto negotiation process as it progresses.
 * The DP83840 is first told to start doing it's thing, we set up the time
 * and place the timer state machine in it's initial state.
 *
 * Here the timer peeks at the DP83840 status registers at each click to see
 * if the auto negotiation has completed, we assume here that the DP83840 PHY
 * will time out at some point and just tell us what (didn't) happen.  For
 * complete coverage we only allow so many of the ticks at this level to run,
 * when this has expired we print a warning message and try another strategy.
 * This "other" strategy is to force the interface into various speed/duplex
 * configurations and we stop when we see a link-up condition before the
 * maximum number of "peek" ticks have occurred.
 *
 * Once a valid link status has been detected we configure the BigMAC and
 * the rest of the Happy Meal to speak the most efficient protocol we could
 * get a clean link for.  The priority for link configurations, highest first
 * is:
 *                 100 Base-T Full Duplex
 *                 100 Base-T Half Duplex
 *                 10 Base-T Full Duplex
 *                 10 Base-T Half Duplex
 *
 * We start a new timer now, after a successful auto negotiation status has
 * been detected.  This timer just waits for the link-up bit to get set in
 * the BMCR of the DP83840.  When this occurs we print a kernel log message
 * describing the link type in use and the fact that it is up.
 *
 * If a fatal error of some sort is signalled and detected in the interrupt
 * service routine, and the chip is reset, or the link is ifconfig'd down
 * and then back up, this entire process repeats itself all over again.
 */
static int try_next_permutation(struct happy_meal *hp, void __iomem *tregs)
{
	hp->sw_bmcr = happy_meal_tcvr_read(hp, tregs, MII_BMCR);

	/* Downgrade from full to half duplex.  Only possible
	 * via ethtool.
	 */
	if (hp->sw_bmcr & BMCR_FULLDPLX) {
		hp->sw_bmcr &= ~(BMCR_FULLDPLX);
		happy_meal_tcvr_write(hp, tregs, MII_BMCR, hp->sw_bmcr);
		return 0;
	}

	/* Downgrade from 100 to 10. */
	if (hp->sw_bmcr & BMCR_SPEED100) {
		hp->sw_bmcr &= ~(BMCR_SPEED100);
		happy_meal_tcvr_write(hp, tregs, MII_BMCR, hp->sw_bmcr);
		return 0;
	}

	/* We've tried everything. */
	return -1;
}

static void display_link_mode(struct happy_meal *hp, void __iomem *tregs)
{
	hp->sw_lpa = happy_meal_tcvr_read(hp, tregs, MII_LPA);

	netdev_info(hp->dev,
		    "Link is up using %s transceiver at %dMb/s, %s Duplex.\n",
		    hp->tcvr_type == external ? "external" : "internal",
		    hp->sw_lpa & (LPA_100HALF | LPA_100FULL) ? 100 : 10,
		    hp->sw_lpa & (LPA_100FULL | LPA_10FULL) ? "Full" : "Half");
}

static void display_forced_link_mode(struct happy_meal *hp, void __iomem *tregs)
{
	hp->sw_bmcr = happy_meal_tcvr_read(hp, tregs, MII_BMCR);

	netdev_info(hp->dev,
		    "Link has been forced up using %s transceiver at %dMb/s, %s Duplex.\n",
		    hp->tcvr_type == external ? "external" : "internal",
		    hp->sw_bmcr & BMCR_SPEED100 ? 100 : 10,
		    hp->sw_bmcr & BMCR_FULLDPLX ? "Full" : "Half");
}

static int set_happy_link_modes(struct happy_meal *hp, void __iomem *tregs)
{
	int full;

	/* All we care about is making sure the bigmac tx_cfg has a
	 * proper duplex setting.
	 */
	if (hp->timer_state == arbwait) {
		hp->sw_lpa = happy_meal_tcvr_read(hp, tregs, MII_LPA);
		if (!(hp->sw_lpa & (LPA_10HALF | LPA_10FULL | LPA_100HALF | LPA_100FULL)))
			goto no_response;
		if (hp->sw_lpa & LPA_100FULL)
			full = 1;
		else if (hp->sw_lpa & LPA_100HALF)
			full = 0;
		else if (hp->sw_lpa & LPA_10FULL)
			full = 1;
		else
			full = 0;
	} else {
		/* Forcing a link mode. */
		hp->sw_bmcr = happy_meal_tcvr_read(hp, tregs, MII_BMCR);
		if (hp->sw_bmcr & BMCR_FULLDPLX)
			full = 1;
		else
			full = 0;
	}

	/* Before changing other bits in the tx_cfg register, and in
	 * general any of other the TX config registers too, you
	 * must:
	 * 1) Clear Enable
	 * 2) Poll with reads until that bit reads back as zero
	 * 3) Make TX configuration changes
	 * 4) Set Enable once more
	 */
	hme_write32(hp, hp->bigmacregs + BMAC_TXCFG,
		    hme_read32(hp, hp->bigmacregs + BMAC_TXCFG) &
		    ~(BIGMAC_TXCFG_ENABLE));
	while (hme_read32(hp, hp->bigmacregs + BMAC_TXCFG) & BIGMAC_TXCFG_ENABLE)
		barrier();
	if (full) {
		hp->happy_flags |= HFLAG_FULL;
		hme_write32(hp, hp->bigmacregs + BMAC_TXCFG,
			    hme_read32(hp, hp->bigmacregs + BMAC_TXCFG) |
			    BIGMAC_TXCFG_FULLDPLX);
	} else {
		hp->happy_flags &= ~(HFLAG_FULL);
		hme_write32(hp, hp->bigmacregs + BMAC_TXCFG,
			    hme_read32(hp, hp->bigmacregs + BMAC_TXCFG) &
			    ~(BIGMAC_TXCFG_FULLDPLX));
	}
	hme_write32(hp, hp->bigmacregs + BMAC_TXCFG,
		    hme_read32(hp, hp->bigmacregs + BMAC_TXCFG) |
		    BIGMAC_TXCFG_ENABLE);
	return 0;
no_response:
	return 1;
}

static int is_lucent_phy(struct happy_meal *hp)
{
	void __iomem *tregs = hp->tcvregs;
	unsigned short mr2, mr3;
	int ret = 0;

	mr2 = happy_meal_tcvr_read(hp, tregs, 2);
	mr3 = happy_meal_tcvr_read(hp, tregs, 3);
	if ((mr2 & 0xffff) == 0x0180 &&
	    ((mr3 & 0xffff) >> 10) == 0x1d)
		ret = 1;

	return ret;
}

/* hp->happy_lock must be held */
static void
happy_meal_begin_auto_negotiation(struct happy_meal *hp,
				  void __iomem *tregs,
				  const struct ethtool_link_ksettings *ep)
{
	int timeout;

	/* Read all of the registers we are interested in now. */
	hp->sw_bmsr      = happy_meal_tcvr_read(hp, tregs, MII_BMSR);
	hp->sw_bmcr      = happy_meal_tcvr_read(hp, tregs, MII_BMCR);
	hp->sw_physid1   = happy_meal_tcvr_read(hp, tregs, MII_PHYSID1);
	hp->sw_physid2   = happy_meal_tcvr_read(hp, tregs, MII_PHYSID2);

	/* XXX Check BMSR_ANEGCAPABLE, should not be necessary though. */

	hp->sw_advertise = happy_meal_tcvr_read(hp, tregs, MII_ADVERTISE);
	if (!ep || ep->base.autoneg == AUTONEG_ENABLE) {
		/* Advertise everything we can support. */
		if (hp->sw_bmsr & BMSR_10HALF)
			hp->sw_advertise |= (ADVERTISE_10HALF);
		else
			hp->sw_advertise &= ~(ADVERTISE_10HALF);

		if (hp->sw_bmsr & BMSR_10FULL)
			hp->sw_advertise |= (ADVERTISE_10FULL);
		else
			hp->sw_advertise &= ~(ADVERTISE_10FULL);
		if (hp->sw_bmsr & BMSR_100HALF)
			hp->sw_advertise |= (ADVERTISE_100HALF);
		else
			hp->sw_advertise &= ~(ADVERTISE_100HALF);
		if (hp->sw_bmsr & BMSR_100FULL)
			hp->sw_advertise |= (ADVERTISE_100FULL);
		else
			hp->sw_advertise &= ~(ADVERTISE_100FULL);
		happy_meal_tcvr_write(hp, tregs, MII_ADVERTISE, hp->sw_advertise);

		/* XXX Currently no Happy Meal cards I know off support 100BaseT4,
		 * XXX and this is because the DP83840 does not support it, changes
		 * XXX would need to be made to the tx/rx logic in the driver as well
		 * XXX so I completely skip checking for it in the BMSR for now.
		 */

		ASD("Advertising [ %s%s%s%s]\n",
		    hp->sw_advertise & ADVERTISE_10HALF ? "10H " : "",
		    hp->sw_advertise & ADVERTISE_10FULL ? "10F " : "",
		    hp->sw_advertise & ADVERTISE_100HALF ? "100H " : "",
		    hp->sw_advertise & ADVERTISE_100FULL ? "100F " : "");

		/* Enable Auto-Negotiation, this is usually on already... */
		hp->sw_bmcr |= BMCR_ANENABLE;
		happy_meal_tcvr_write(hp, tregs, MII_BMCR, hp->sw_bmcr);

		/* Restart it to make sure it is going. */
		hp->sw_bmcr |= BMCR_ANRESTART;
		happy_meal_tcvr_write(hp, tregs, MII_BMCR, hp->sw_bmcr);

		/* BMCR_ANRESTART self clears when the process has begun. */

		timeout = 64;  /* More than enough. */
		while (--timeout) {
			hp->sw_bmcr = happy_meal_tcvr_read(hp, tregs, MII_BMCR);
			if (!(hp->sw_bmcr & BMCR_ANRESTART))
				break; /* got it. */
			udelay(10);
		}
		if (!timeout) {
			netdev_err(hp->dev,
				   "Happy Meal would not start auto negotiation BMCR=0x%04x\n",
				   hp->sw_bmcr);
			netdev_notice(hp->dev,
				      "Performing force link detection.\n");
			goto force_link;
		} else {
			hp->timer_state = arbwait;
		}
	} else {
force_link:
		/* Force the link up, trying first a particular mode.
		 * Either we are here at the request of ethtool or
		 * because the Happy Meal would not start to autoneg.
		 */

		/* Disable auto-negotiation in BMCR, enable the duplex and
		 * speed setting, init the timer state machine, and fire it off.
		 */
		if (!ep || ep->base.autoneg == AUTONEG_ENABLE) {
			hp->sw_bmcr = BMCR_SPEED100;
		} else {
			if (ep->base.speed == SPEED_100)
				hp->sw_bmcr = BMCR_SPEED100;
			else
				hp->sw_bmcr = 0;
			if (ep->base.duplex == DUPLEX_FULL)
				hp->sw_bmcr |= BMCR_FULLDPLX;
		}
		happy_meal_tcvr_write(hp, tregs, MII_BMCR, hp->sw_bmcr);

		if (!is_lucent_phy(hp)) {
			/* OK, seems we need do disable the transceiver for the first
			 * tick to make sure we get an accurate link state at the
			 * second tick.
			 */
			hp->sw_csconfig = happy_meal_tcvr_read(hp, tregs,
							       DP83840_CSCONFIG);
			hp->sw_csconfig &= ~(CSCONFIG_TCVDISAB);
			happy_meal_tcvr_write(hp, tregs, DP83840_CSCONFIG,
					      hp->sw_csconfig);
		}
		hp->timer_state = ltrywait;
	}

	hp->timer_ticks = 0;
	hp->happy_timer.expires = jiffies + (12 * HZ)/10;  /* 1.2 sec. */
	add_timer(&hp->happy_timer);
}

static void happy_meal_timer(struct timer_list *t)
{
	struct happy_meal *hp = timer_container_of(hp, t, happy_timer);
	void __iomem *tregs = hp->tcvregs;
	int restart_timer = 0;

	spin_lock_irq(&hp->happy_lock);

	hp->timer_ticks++;
	switch(hp->timer_state) {
	case arbwait:
		/* Only allow for 5 ticks, thats 10 seconds and much too
		 * long to wait for arbitration to complete.
		 */
		if (hp->timer_ticks >= 10) {
			/* Enter force mode. */
	do_force_mode:
			hp->sw_bmcr = happy_meal_tcvr_read(hp, tregs, MII_BMCR);
			netdev_notice(hp->dev,
				      "Auto-Negotiation unsuccessful, trying force link mode\n");
			hp->sw_bmcr = BMCR_SPEED100;
			happy_meal_tcvr_write(hp, tregs, MII_BMCR, hp->sw_bmcr);

			if (!is_lucent_phy(hp)) {
				/* OK, seems we need do disable the transceiver for the first
				 * tick to make sure we get an accurate link state at the
				 * second tick.
				 */
				hp->sw_csconfig = happy_meal_tcvr_read(hp, tregs, DP83840_CSCONFIG);
				hp->sw_csconfig &= ~(CSCONFIG_TCVDISAB);
				happy_meal_tcvr_write(hp, tregs, DP83840_CSCONFIG, hp->sw_csconfig);
			}
			hp->timer_state = ltrywait;
			hp->timer_ticks = 0;
			restart_timer = 1;
		} else {
			/* Anything interesting happen? */
			hp->sw_bmsr = happy_meal_tcvr_read(hp, tregs, MII_BMSR);
			if (hp->sw_bmsr & BMSR_ANEGCOMPLETE) {
				int ret;

				/* Just what we've been waiting for... */
				ret = set_happy_link_modes(hp, tregs);
				if (ret) {
					/* Ooops, something bad happened, go to force
					 * mode.
					 *
					 * XXX Broken hubs which don't support 802.3u
					 * XXX auto-negotiation make this happen as well.
					 */
					goto do_force_mode;
				}

				/* Success, at least so far, advance our state engine. */
				hp->timer_state = lupwait;
				restart_timer = 1;
			} else {
				restart_timer = 1;
			}
		}
		break;

	case lupwait:
		/* Auto negotiation was successful and we are awaiting a
		 * link up status.  I have decided to let this timer run
		 * forever until some sort of error is signalled, reporting
		 * a message to the user at 10 second intervals.
		 */
		hp->sw_bmsr = happy_meal_tcvr_read(hp, tregs, MII_BMSR);
		if (hp->sw_bmsr & BMSR_LSTATUS) {
			/* Wheee, it's up, display the link mode in use and put
			 * the timer to sleep.
			 */
			display_link_mode(hp, tregs);
			hp->timer_state = asleep;
			restart_timer = 0;
		} else {
			if (hp->timer_ticks >= 10) {
				netdev_notice(hp->dev,
					      "Auto negotiation successful, link still not completely up.\n");
				hp->timer_ticks = 0;
				restart_timer = 1;
			} else {
				restart_timer = 1;
			}
		}
		break;

	case ltrywait:
		/* Making the timeout here too long can make it take
		 * annoyingly long to attempt all of the link mode
		 * permutations, but then again this is essentially
		 * error recovery code for the most part.
		 */
		hp->sw_bmsr = happy_meal_tcvr_read(hp, tregs, MII_BMSR);
		hp->sw_csconfig = happy_meal_tcvr_read(hp, tregs, DP83840_CSCONFIG);
		if (hp->timer_ticks == 1) {
			if (!is_lucent_phy(hp)) {
				/* Re-enable transceiver, we'll re-enable the transceiver next
				 * tick, then check link state on the following tick.
				 */
				hp->sw_csconfig |= CSCONFIG_TCVDISAB;
				happy_meal_tcvr_write(hp, tregs,
						      DP83840_CSCONFIG, hp->sw_csconfig);
			}
			restart_timer = 1;
			break;
		}
		if (hp->timer_ticks == 2) {
			if (!is_lucent_phy(hp)) {
				hp->sw_csconfig &= ~(CSCONFIG_TCVDISAB);
				happy_meal_tcvr_write(hp, tregs,
						      DP83840_CSCONFIG, hp->sw_csconfig);
			}
			restart_timer = 1;
			break;
		}
		if (hp->sw_bmsr & BMSR_LSTATUS) {
			/* Force mode selection success. */
			display_forced_link_mode(hp, tregs);
			set_happy_link_modes(hp, tregs); /* XXX error? then what? */
			hp->timer_state = asleep;
			restart_timer = 0;
		} else {
			if (hp->timer_ticks >= 4) { /* 6 seconds or so... */
				int ret;

				ret = try_next_permutation(hp, tregs);
				if (ret == -1) {
					/* Aieee, tried them all, reset the
					 * chip and try all over again.
					 */

					/* Let the user know... */
					netdev_notice(hp->dev,
						      "Link down, cable problem?\n");

					happy_meal_begin_auto_negotiation(hp, tregs, NULL);
					goto out;
				}
				if (!is_lucent_phy(hp)) {
					hp->sw_csconfig = happy_meal_tcvr_read(hp, tregs,
									       DP83840_CSCONFIG);
					hp->sw_csconfig |= CSCONFIG_TCVDISAB;
					happy_meal_tcvr_write(hp, tregs,
							      DP83840_CSCONFIG, hp->sw_csconfig);
				}
				hp->timer_ticks = 0;
				restart_timer = 1;
			} else {
				restart_timer = 1;
			}
		}
		break;

	case asleep:
	default:
		/* Can't happens.... */
		netdev_err(hp->dev,
			   "Aieee, link timer is asleep but we got one anyways!\n");
		restart_timer = 0;
		hp->timer_ticks = 0;
		hp->timer_state = asleep; /* foo on you */
		break;
	}

	if (restart_timer) {
		hp->happy_timer.expires = jiffies + ((12 * HZ)/10); /* 1.2 sec. */
		add_timer(&hp->happy_timer);
	}

out:
	spin_unlock_irq(&hp->happy_lock);
}

#define TX_RESET_TRIES     32
#define RX_RESET_TRIES     32

/* hp->happy_lock must be held */
static void happy_meal_tx_reset(struct happy_meal *hp, void __iomem *bregs)
{
	int tries = TX_RESET_TRIES;

	HMD("reset...\n");

	/* Would you like to try our SMCC Delux? */
	hme_write32(hp, bregs + BMAC_TXSWRESET, 0);
	while ((hme_read32(hp, bregs + BMAC_TXSWRESET) & 1) && --tries)
		udelay(20);

	/* Lettuce, tomato, buggy hardware (no extra charge)? */
	if (!tries)
		netdev_err(hp->dev, "Transceiver BigMac ATTACK!");

	/* Take care. */
	HMD("done\n");
}

/* hp->happy_lock must be held */
static void happy_meal_rx_reset(struct happy_meal *hp, void __iomem *bregs)
{
	int tries = RX_RESET_TRIES;

	HMD("reset...\n");

	/* We have a special on GNU/Viking hardware bugs today. */
	hme_write32(hp, bregs + BMAC_RXSWRESET, 0);
	while ((hme_read32(hp, bregs + BMAC_RXSWRESET) & 1) && --tries)
		udelay(20);

	/* Will that be all? */
	if (!tries)
		netdev_err(hp->dev, "Receiver BigMac ATTACK!\n");

	/* Don't forget your vik_1137125_wa.  Have a nice day. */
	HMD("done\n");
}

#define STOP_TRIES         16

/* hp->happy_lock must be held */
static void happy_meal_stop(struct happy_meal *hp, void __iomem *gregs)
{
	int tries = STOP_TRIES;

	HMD("reset...\n");

	/* We're consolidating our STB products, it's your lucky day. */
	hme_write32(hp, gregs + GREG_SWRESET, GREG_RESET_ALL);
	while (hme_read32(hp, gregs + GREG_SWRESET) && --tries)
		udelay(20);

	/* Come back next week when we are "Sun Microelectronics". */
	if (!tries)
		netdev_err(hp->dev, "Fry guys.\n");

	/* Remember: "Different name, same old buggy as shit hardware." */
	HMD("done\n");
}

/* hp->happy_lock must be held */
static void happy_meal_get_counters(struct happy_meal *hp, void __iomem *bregs)
{
	struct net_device_stats *stats = &hp->dev->stats;

	stats->rx_crc_errors += hme_read32(hp, bregs + BMAC_RCRCECTR);
	hme_write32(hp, bregs + BMAC_RCRCECTR, 0);

	stats->rx_frame_errors += hme_read32(hp, bregs + BMAC_UNALECTR);
	hme_write32(hp, bregs + BMAC_UNALECTR, 0);

	stats->rx_length_errors += hme_read32(hp, bregs + BMAC_GLECTR);
	hme_write32(hp, bregs + BMAC_GLECTR, 0);

	stats->tx_aborted_errors += hme_read32(hp, bregs + BMAC_EXCTR);

	stats->collisions +=
		(hme_read32(hp, bregs + BMAC_EXCTR) +
		 hme_read32(hp, bregs + BMAC_LTCTR));
	hme_write32(hp, bregs + BMAC_EXCTR, 0);
	hme_write32(hp, bregs + BMAC_LTCTR, 0);
}

/* Only Sun can take such nice parts and fuck up the programming interface
 * like this.  Good job guys...
 */
#define TCVR_RESET_TRIES       16 /* It should reset quickly        */
#define TCVR_UNISOLATE_TRIES   32 /* Dis-isolation can take longer. */

/* hp->happy_lock must be held */
static int happy_meal_tcvr_reset(struct happy_meal *hp, void __iomem *tregs)
{
	u32 tconfig;
	int result, tries = TCVR_RESET_TRIES;

	tconfig = hme_read32(hp, tregs + TCVR_CFG);
	ASD("tcfg=%08x\n", tconfig);
	if (hp->tcvr_type == external) {
		hme_write32(hp, tregs + TCVR_CFG, tconfig & ~(TCV_CFG_PSELECT));
		hp->tcvr_type = internal;
		hp->paddr = TCV_PADDR_ITX;
		happy_meal_tcvr_write(hp, tregs, MII_BMCR,
				      (BMCR_LOOPBACK|BMCR_PDOWN|BMCR_ISOLATE));
		result = happy_meal_tcvr_read(hp, tregs, MII_BMCR);
		if (result == TCVR_FAILURE) {
			ASD("phyread_fail\n");
			return -1;
		}
		ASD("external: ISOLATE, phyread_ok, PSELECT\n");
		hme_write32(hp, tregs + TCVR_CFG, tconfig | TCV_CFG_PSELECT);
		hp->tcvr_type = external;
		hp->paddr = TCV_PADDR_ETX;
	} else {
		if (tconfig & TCV_CFG_MDIO1) {
			hme_write32(hp, tregs + TCVR_CFG, (tconfig | TCV_CFG_PSELECT));
			happy_meal_tcvr_write(hp, tregs, MII_BMCR,
					      (BMCR_LOOPBACK|BMCR_PDOWN|BMCR_ISOLATE));
			result = happy_meal_tcvr_read(hp, tregs, MII_BMCR);
			if (result == TCVR_FAILURE) {
				ASD("phyread_fail>\n");
				return -1;
			}
			ASD("internal: PSELECT, ISOLATE, phyread_ok, ~PSELECT\n");
			hme_write32(hp, tregs + TCVR_CFG, (tconfig & ~(TCV_CFG_PSELECT)));
			hp->tcvr_type = internal;
			hp->paddr = TCV_PADDR_ITX;
		}
	}

	ASD("BMCR_RESET...\n");
	happy_meal_tcvr_write(hp, tregs, MII_BMCR, BMCR_RESET);

	while (--tries) {
		result = happy_meal_tcvr_read(hp, tregs, MII_BMCR);
		if (result == TCVR_FAILURE)
			return -1;
		hp->sw_bmcr = result;
		if (!(result & BMCR_RESET))
			break;
		udelay(20);
	}
	if (!tries) {
		ASD("BMCR RESET FAILED!\n");
		return -1;
	}
	ASD("RESET_OK\n");

	/* Get fresh copies of the PHY registers. */
	hp->sw_bmsr      = happy_meal_tcvr_read(hp, tregs, MII_BMSR);
	hp->sw_physid1   = happy_meal_tcvr_read(hp, tregs, MII_PHYSID1);
	hp->sw_physid2   = happy_meal_tcvr_read(hp, tregs, MII_PHYSID2);
	hp->sw_advertise = happy_meal_tcvr_read(hp, tregs, MII_ADVERTISE);

	ASD("UNISOLATE...\n");
	hp->sw_bmcr &= ~(BMCR_ISOLATE);
	happy_meal_tcvr_write(hp, tregs, MII_BMCR, hp->sw_bmcr);

	tries = TCVR_UNISOLATE_TRIES;
	while (--tries) {
		result = happy_meal_tcvr_read(hp, tregs, MII_BMCR);
		if (result == TCVR_FAILURE)
			return -1;
		if (!(result & BMCR_ISOLATE))
			break;
		udelay(20);
	}
	if (!tries) {
		ASD("UNISOLATE FAILED!\n");
		return -1;
	}
	ASD("SUCCESS and CSCONFIG_DFBYPASS\n");
	if (!is_lucent_phy(hp)) {
		result = happy_meal_tcvr_read(hp, tregs,
					      DP83840_CSCONFIG);
		happy_meal_tcvr_write(hp, tregs,
				      DP83840_CSCONFIG, (result | CSCONFIG_DFBYPASS));
	}
	return 0;
}

/* Figure out whether we have an internal or external transceiver.
 *
 * hp->happy_lock must be held
 */
static void happy_meal_transceiver_check(struct happy_meal *hp, void __iomem *tregs)
{
	unsigned long tconfig = hme_read32(hp, tregs + TCVR_CFG);
	u32 reread = hme_read32(hp, tregs + TCVR_CFG);

	ASD("tcfg=%08lx\n", tconfig);
	if (reread & TCV_CFG_MDIO1) {
		hme_write32(hp, tregs + TCVR_CFG, tconfig | TCV_CFG_PSELECT);
		hp->paddr = TCV_PADDR_ETX;
		hp->tcvr_type = external;
		ASD("not polling, external\n");
	} else {
		if (reread & TCV_CFG_MDIO0) {
			hme_write32(hp, tregs + TCVR_CFG,
				    tconfig & ~(TCV_CFG_PSELECT));
			hp->paddr = TCV_PADDR_ITX;
			hp->tcvr_type = internal;
			ASD("not polling, internal\n");
		} else {
			netdev_err(hp->dev,
				   "Transceiver and a coke please.");
			hp->tcvr_type = none; /* Grrr... */
			ASD("not polling, none\n");
		}
	}
}

/* The receive ring buffers are a bit tricky to get right.  Here goes...
 *
 * The buffers we dma into must be 64 byte aligned.  So we use a special
 * alloc_skb() routine for the happy meal to allocate 64 bytes more than
 * we really need.
 *
 * We use skb_reserve() to align the data block we get in the skb.  We
 * also program the etxregs->cfg register to use an offset of 2.  This
 * imperical constant plus the ethernet header size will always leave
 * us with a nicely aligned ip header once we pass things up to the
 * protocol layers.
 *
 * The numbers work out to:
 *
 *         Max ethernet frame size         1518
 *         Ethernet header size              14
 *         Happy Meal base offset             2
 *
 * Say a skb data area is at 0xf001b010, and its size alloced is
 * (ETH_FRAME_LEN + 64 + 2) = (1514 + 64 + 2) = 1580 bytes.
 *
 * First our alloc_skb() routine aligns the data base to a 64 byte
 * boundary.  We now have 0xf001b040 as our skb data address.  We
 * plug this into the receive descriptor address.
 *
 * Next, we skb_reserve() 2 bytes to account for the Happy Meal offset.
 * So now the data we will end up looking at starts at 0xf001b042.  When
 * the packet arrives, we will check out the size received and subtract
 * this from the skb->length.  Then we just pass the packet up to the
 * protocols as is, and allocate a new skb to replace this slot we have
 * just received from.
 *
 * The ethernet layer will strip the ether header from the front of the
 * skb we just sent to it, this leaves us with the ip header sitting
 * nicely aligned at 0xf001b050.  Also, for tcp and udp packets the
 * Happy Meal has even checksummed the tcp/udp data for us.  The 16
 * bit checksum is obtained from the low bits of the receive descriptor
 * flags, thus:
 *
 * 	skb->csum = rxd->rx_flags & 0xffff;
 * 	skb->ip_summed = CHECKSUM_COMPLETE;
 *
 * before sending off the skb to the protocols, and we are good as gold.
 */
static void happy_meal_clean_rings(struct happy_meal *hp)
{
	int i;

	for (i = 0; i < RX_RING_SIZE; i++) {
		if (hp->rx_skbs[i] != NULL) {
			struct sk_buff *skb = hp->rx_skbs[i];
			struct happy_meal_rxd *rxd;
			u32 dma_addr;

			rxd = &hp->happy_block->happy_meal_rxd[i];
			dma_addr = hme_read_desc32(hp, &rxd->rx_addr);
			dma_unmap_single(hp->dma_dev, dma_addr,
					 RX_BUF_ALLOC_SIZE, DMA_FROM_DEVICE);
			dev_kfree_skb_any(skb);
			hp->rx_skbs[i] = NULL;
		}
	}

	for (i = 0; i < TX_RING_SIZE; i++) {
		if (hp->tx_skbs[i] != NULL) {
			struct sk_buff *skb = hp->tx_skbs[i];
			struct happy_meal_txd *txd;
			u32 dma_addr;
			int frag;

			hp->tx_skbs[i] = NULL;

			for (frag = 0; frag <= skb_shinfo(skb)->nr_frags; frag++) {
				txd = &hp->happy_block->happy_meal_txd[i];
				dma_addr = hme_read_desc32(hp, &txd->tx_addr);
				if (!frag)
					dma_unmap_single(hp->dma_dev, dma_addr,
							 (hme_read_desc32(hp, &txd->tx_flags)
							  & TXFLAG_SIZE),
							 DMA_TO_DEVICE);
				else
					dma_unmap_page(hp->dma_dev, dma_addr,
							 (hme_read_desc32(hp, &txd->tx_flags)
							  & TXFLAG_SIZE),
							 DMA_TO_DEVICE);

				if (frag != skb_shinfo(skb)->nr_frags)
					i++;
			}

			dev_kfree_skb_any(skb);
		}
	}
}

/* hp->happy_lock must be held */
static void happy_meal_init_rings(struct happy_meal *hp)
{
	struct hmeal_init_block *hb = hp->happy_block;
	int i;

	HMD("counters to zero\n");
	hp->rx_new = hp->rx_old = hp->tx_new = hp->tx_old = 0;

	/* Free any skippy bufs left around in the rings. */
	happy_meal_clean_rings(hp);

	/* Now get new skippy bufs for the receive ring. */
	HMD("init rxring\n");
	for (i = 0; i < RX_RING_SIZE; i++) {
		struct sk_buff *skb;
		u32 mapping;

		skb = happy_meal_alloc_skb(RX_BUF_ALLOC_SIZE, GFP_ATOMIC);
		if (!skb) {
			hme_write_rxd(hp, &hb->happy_meal_rxd[i], 0, 0);
			continue;
		}
		hp->rx_skbs[i] = skb;

		/* Because we reserve afterwards. */
		skb_put(skb, (ETH_FRAME_LEN + RX_OFFSET + 4));
		mapping = dma_map_single(hp->dma_dev, skb->data, RX_BUF_ALLOC_SIZE,
					 DMA_FROM_DEVICE);
		if (dma_mapping_error(hp->dma_dev, mapping)) {
			dev_kfree_skb_any(skb);
			hme_write_rxd(hp, &hb->happy_meal_rxd[i], 0, 0);
			continue;
		}
		hme_write_rxd(hp, &hb->happy_meal_rxd[i],
			      (RXFLAG_OWN | ((RX_BUF_ALLOC_SIZE - RX_OFFSET) << 16)),
			      mapping);
		skb_reserve(skb, RX_OFFSET);
	}

	HMD("init txring\n");
	for (i = 0; i < TX_RING_SIZE; i++)
		hme_write_txd(hp, &hb->happy_meal_txd[i], 0, 0);

	HMD("done\n");
}

/* hp->happy_lock must be held */
static int happy_meal_init(struct happy_meal *hp)
{
	const unsigned char *e = &hp->dev->dev_addr[0];
	void __iomem *gregs        = hp->gregs;
	void __iomem *etxregs      = hp->etxregs;
	void __iomem *erxregs      = hp->erxregs;
	void __iomem *bregs        = hp->bigmacregs;
	void __iomem *tregs        = hp->tcvregs;
	const char *bursts = "64";
	u32 regtmp, rxcfg;

	/* If auto-negotiation timer is running, kill it. */
	timer_delete(&hp->happy_timer);

	HMD("happy_flags[%08x]\n", hp->happy_flags);
	if (!(hp->happy_flags & HFLAG_INIT)) {
		HMD("set HFLAG_INIT\n");
		hp->happy_flags |= HFLAG_INIT;
		happy_meal_get_counters(hp, bregs);
	}

	/* Stop transmitter and receiver. */
	HMD("to happy_meal_stop\n");
	happy_meal_stop(hp, gregs);

	/* Alloc and reset the tx/rx descriptor chains. */
	HMD("to happy_meal_init_rings\n");
	happy_meal_init_rings(hp);

	/* See if we can enable the MIF frame on this card to speak to the DP83840. */
	if (hp->happy_flags & HFLAG_FENABLE) {
		HMD("use frame old[%08x]\n",
		    hme_read32(hp, tregs + TCVR_CFG));
		hme_write32(hp, tregs + TCVR_CFG,
			    hme_read32(hp, tregs + TCVR_CFG) & ~(TCV_CFG_BENABLE));
	} else {
		HMD("use bitbang old[%08x]\n",
		    hme_read32(hp, tregs + TCVR_CFG));
		hme_write32(hp, tregs + TCVR_CFG,
			    hme_read32(hp, tregs + TCVR_CFG) | TCV_CFG_BENABLE);
	}

	/* Check the state of the transceiver. */
	HMD("to happy_meal_transceiver_check\n");
	happy_meal_transceiver_check(hp, tregs);

	/* Put the Big Mac into a sane state. */
	switch(hp->tcvr_type) {
	case none:
		/* Cannot operate if we don't know the transceiver type! */
		HMD("AAIEEE no transceiver type, EAGAIN\n");
		return -EAGAIN;

	case internal:
		/* Using the MII buffers. */
		HMD("internal, using MII\n");
		hme_write32(hp, bregs + BMAC_XIFCFG, 0);
		break;

	case external:
		/* Not using the MII, disable it. */
		HMD("external, disable MII\n");
		hme_write32(hp, bregs + BMAC_XIFCFG, BIGMAC_XCFG_MIIDISAB);
		break;
	}

	if (happy_meal_tcvr_reset(hp, tregs))
		return -EAGAIN;

	/* Reset the Happy Meal Big Mac transceiver and the receiver. */
	HMD("tx/rx reset\n");
	happy_meal_tx_reset(hp, bregs);
	happy_meal_rx_reset(hp, bregs);

	/* Set jam size and inter-packet gaps to reasonable defaults. */
	hme_write32(hp, bregs + BMAC_JSIZE, DEFAULT_JAMSIZE);
	hme_write32(hp, bregs + BMAC_IGAP1, DEFAULT_IPG1);
	hme_write32(hp, bregs + BMAC_IGAP2, DEFAULT_IPG2);

	/* Load up the MAC address and random seed. */

	/* The docs recommend to use the 10LSB of our MAC here. */
	hme_write32(hp, bregs + BMAC_RSEED, ((e[5] | e[4]<<8)&0x3ff));

	hme_write32(hp, bregs + BMAC_MACADDR2, ((e[4] << 8) | e[5]));
	hme_write32(hp, bregs + BMAC_MACADDR1, ((e[2] << 8) | e[3]));
	hme_write32(hp, bregs + BMAC_MACADDR0, ((e[0] << 8) | e[1]));

	if ((hp->dev->flags & IFF_ALLMULTI) ||
	    (netdev_mc_count(hp->dev) > 64)) {
		hme_write32(hp, bregs + BMAC_HTABLE0, 0xffff);
		hme_write32(hp, bregs + BMAC_HTABLE1, 0xffff);
		hme_write32(hp, bregs + BMAC_HTABLE2, 0xffff);
		hme_write32(hp, bregs + BMAC_HTABLE3, 0xffff);
	} else if ((hp->dev->flags & IFF_PROMISC) == 0) {
		u16 hash_table[4];
		struct netdev_hw_addr *ha;
		u32 crc;

		memset(hash_table, 0, sizeof(hash_table));
		netdev_for_each_mc_addr(ha, hp->dev) {
			crc = ether_crc_le(6, ha->addr);
			crc >>= 26;
			hash_table[crc >> 4] |= 1 << (crc & 0xf);
		}
		hme_write32(hp, bregs + BMAC_HTABLE0, hash_table[0]);
		hme_write32(hp, bregs + BMAC_HTABLE1, hash_table[1]);
		hme_write32(hp, bregs + BMAC_HTABLE2, hash_table[2]);
		hme_write32(hp, bregs + BMAC_HTABLE3, hash_table[3]);
	} else {
		hme_write32(hp, bregs + BMAC_HTABLE3, 0);
		hme_write32(hp, bregs + BMAC_HTABLE2, 0);
		hme_write32(hp, bregs + BMAC_HTABLE1, 0);
		hme_write32(hp, bregs + BMAC_HTABLE0, 0);
	}

	/* Set the RX and TX ring ptrs. */
	HMD("ring ptrs rxr[%08x] txr[%08x]\n",
	    ((__u32)hp->hblock_dvma + hblock_offset(happy_meal_rxd, 0)),
	    ((__u32)hp->hblock_dvma + hblock_offset(happy_meal_txd, 0)));
	hme_write32(hp, erxregs + ERX_RING,
		    ((__u32)hp->hblock_dvma + hblock_offset(happy_meal_rxd, 0)));
	hme_write32(hp, etxregs + ETX_RING,
		    ((__u32)hp->hblock_dvma + hblock_offset(happy_meal_txd, 0)));

	/* Parity issues in the ERX unit of some HME revisions can cause some
	 * registers to not be written unless their parity is even.  Detect such
	 * lost writes and simply rewrite with a low bit set (which will be ignored
	 * since the rxring needs to be 2K aligned).
	 */
	if (hme_read32(hp, erxregs + ERX_RING) !=
	    ((__u32)hp->hblock_dvma + hblock_offset(happy_meal_rxd, 0)))
		hme_write32(hp, erxregs + ERX_RING,
			    ((__u32)hp->hblock_dvma + hblock_offset(happy_meal_rxd, 0))
			    | 0x4);

	/* Set the supported burst sizes. */
#ifndef CONFIG_SPARC
	/* It is always PCI and can handle 64byte bursts. */
	hme_write32(hp, gregs + GREG_CFG, GREG_CFG_BURST64);
#else
	if ((hp->happy_bursts & DMA_BURST64) &&
	    ((hp->happy_flags & HFLAG_PCI) != 0
#ifdef CONFIG_SBUS
	     || sbus_can_burst64()
#endif
	     || 0)) {
		u32 gcfg = GREG_CFG_BURST64;

		/* I have no idea if I should set the extended
		 * transfer mode bit for Cheerio, so for now I
		 * do not.  -DaveM
		 */
#ifdef CONFIG_SBUS
		if ((hp->happy_flags & HFLAG_PCI) == 0) {
			struct platform_device *op = hp->happy_dev;
			if (sbus_can_dma_64bit()) {
				sbus_set_sbus64(&op->dev,
						hp->happy_bursts);
				gcfg |= GREG_CFG_64BIT;
			}
		}
#endif

		bursts = "64";
		hme_write32(hp, gregs + GREG_CFG, gcfg);
	} else if (hp->happy_bursts & DMA_BURST32) {
		bursts = "32";
		hme_write32(hp, gregs + GREG_CFG, GREG_CFG_BURST32);
	} else if (hp->happy_bursts & DMA_BURST16) {
		bursts = "16";
		hme_write32(hp, gregs + GREG_CFG, GREG_CFG_BURST16);
	} else {
		bursts = "XXX";
		hme_write32(hp, gregs + GREG_CFG, 0);
	}
#endif /* CONFIG_SPARC */

	HMD("old[%08x] bursts<%s>\n",
	    hme_read32(hp, gregs + GREG_CFG), bursts);

	/* Turn off interrupts we do not want to hear. */
	hme_write32(hp, gregs + GREG_IMASK,
		    (GREG_IMASK_GOTFRAME | GREG_IMASK_RCNTEXP |
		     GREG_IMASK_SENTFRAME | GREG_IMASK_TXPERR));

	/* Set the transmit ring buffer size. */
	HMD("tx rsize=%d oreg[%08x]\n", (int)TX_RING_SIZE,
	    hme_read32(hp, etxregs + ETX_RSIZE));
	hme_write32(hp, etxregs + ETX_RSIZE, (TX_RING_SIZE >> ETX_RSIZE_SHIFT) - 1);

	/* Enable transmitter DVMA. */
	HMD("tx dma enable old[%08x]\n", hme_read32(hp, etxregs + ETX_CFG));
	hme_write32(hp, etxregs + ETX_CFG,
		    hme_read32(hp, etxregs + ETX_CFG) | ETX_CFG_DMAENABLE);

	/* This chip really rots, for the receiver sometimes when you
	 * write to its control registers not all the bits get there
	 * properly.  I cannot think of a sane way to provide complete
	 * coverage for this hardware bug yet.
	 */
	HMD("erx regs bug old[%08x]\n",
	    hme_read32(hp, erxregs + ERX_CFG));
	hme_write32(hp, erxregs + ERX_CFG, ERX_CFG_DEFAULT(RX_OFFSET));
	regtmp = hme_read32(hp, erxregs + ERX_CFG);
	hme_write32(hp, erxregs + ERX_CFG, ERX_CFG_DEFAULT(RX_OFFSET));
	if (hme_read32(hp, erxregs + ERX_CFG) != ERX_CFG_DEFAULT(RX_OFFSET)) {
		netdev_err(hp->dev,
			   "Eieee, rx config register gets greasy fries.\n");
		netdev_err(hp->dev,
			   "Trying to set %08x, reread gives %08x\n",
			   ERX_CFG_DEFAULT(RX_OFFSET), regtmp);
		/* XXX Should return failure here... */
	}

	/* Enable Big Mac hash table filter. */
	HMD("enable hash rx_cfg_old[%08x]\n",
	    hme_read32(hp, bregs + BMAC_RXCFG));
	rxcfg = BIGMAC_RXCFG_HENABLE | BIGMAC_RXCFG_REJME;
	if (hp->dev->flags & IFF_PROMISC)
		rxcfg |= BIGMAC_RXCFG_PMISC;
	hme_write32(hp, bregs + BMAC_RXCFG, rxcfg);

	/* Let the bits settle in the chip. */
	udelay(10);

	/* Ok, configure the Big Mac transmitter. */
	HMD("BIGMAC init\n");
	regtmp = 0;
	if (hp->happy_flags & HFLAG_FULL)
		regtmp |= BIGMAC_TXCFG_FULLDPLX;

	/* Don't turn on the "don't give up" bit for now.  It could cause hme
	 * to deadlock with the PHY if a Jabber occurs.
	 */
	hme_write32(hp, bregs + BMAC_TXCFG, regtmp /*| BIGMAC_TXCFG_DGIVEUP*/);

	/* Give up after 16 TX attempts. */
	hme_write32(hp, bregs + BMAC_ALIMIT, 16);

	/* Enable the output drivers no matter what. */
	regtmp = BIGMAC_XCFG_ODENABLE;

	/* If card can do lance mode, enable it. */
	if (hp->happy_flags & HFLAG_LANCE)
		regtmp |= (DEFAULT_IPG0 << 5) | BIGMAC_XCFG_LANCE;

	/* Disable the MII buffers if using external transceiver. */
	if (hp->tcvr_type == external)
		regtmp |= BIGMAC_XCFG_MIIDISAB;

	HMD("XIF config old[%08x]\n", hme_read32(hp, bregs + BMAC_XIFCFG));
	hme_write32(hp, bregs + BMAC_XIFCFG, regtmp);

	/* Start things up. */
	HMD("tx old[%08x] and rx [%08x] ON!\n",
	    hme_read32(hp, bregs + BMAC_TXCFG),
	    hme_read32(hp, bregs + BMAC_RXCFG));

	/* Set larger TX/RX size to allow for 802.1q */
	hme_write32(hp, bregs + BMAC_TXMAX, ETH_FRAME_LEN + 8);
	hme_write32(hp, bregs + BMAC_RXMAX, ETH_FRAME_LEN + 8);

	hme_write32(hp, bregs + BMAC_TXCFG,
		    hme_read32(hp, bregs + BMAC_TXCFG) | BIGMAC_TXCFG_ENABLE);
	hme_write32(hp, bregs + BMAC_RXCFG,
		    hme_read32(hp, bregs + BMAC_RXCFG) | BIGMAC_RXCFG_ENABLE);

	/* Get the autonegotiation started, and the watch timer ticking. */
	happy_meal_begin_auto_negotiation(hp, tregs, NULL);

	/* Success. */
	return 0;
}

/* hp->happy_lock must be held */
static void happy_meal_set_initial_advertisement(struct happy_meal *hp)
{
	void __iomem *tregs	= hp->tcvregs;
	void __iomem *bregs	= hp->bigmacregs;
	void __iomem *gregs	= hp->gregs;

	happy_meal_stop(hp, gregs);
	if (hp->happy_flags & HFLAG_FENABLE)
		hme_write32(hp, tregs + TCVR_CFG,
			    hme_read32(hp, tregs + TCVR_CFG) & ~(TCV_CFG_BENABLE));
	else
		hme_write32(hp, tregs + TCVR_CFG,
			    hme_read32(hp, tregs + TCVR_CFG) | TCV_CFG_BENABLE);
	happy_meal_transceiver_check(hp, tregs);
	switch(hp->tcvr_type) {
	case none:
		return;
	case internal:
		hme_write32(hp, bregs + BMAC_XIFCFG, 0);
		break;
	case external:
		hme_write32(hp, bregs + BMAC_XIFCFG, BIGMAC_XCFG_MIIDISAB);
		break;
	}
	if (happy_meal_tcvr_reset(hp, tregs))
		return;

	/* Latch PHY registers as of now. */
	hp->sw_bmsr      = happy_meal_tcvr_read(hp, tregs, MII_BMSR);
	hp->sw_advertise = happy_meal_tcvr_read(hp, tregs, MII_ADVERTISE);

	/* Advertise everything we can support. */
	if (hp->sw_bmsr & BMSR_10HALF)
		hp->sw_advertise |= (ADVERTISE_10HALF);
	else
		hp->sw_advertise &= ~(ADVERTISE_10HALF);

	if (hp->sw_bmsr & BMSR_10FULL)
		hp->sw_advertise |= (ADVERTISE_10FULL);
	else
		hp->sw_advertise &= ~(ADVERTISE_10FULL);
	if (hp->sw_bmsr & BMSR_100HALF)
		hp->sw_advertise |= (ADVERTISE_100HALF);
	else
		hp->sw_advertise &= ~(ADVERTISE_100HALF);
	if (hp->sw_bmsr & BMSR_100FULL)
		hp->sw_advertise |= (ADVERTISE_100FULL);
	else
		hp->sw_advertise &= ~(ADVERTISE_100FULL);

	/* Update the PHY advertisement register. */
	happy_meal_tcvr_write(hp, tregs, MII_ADVERTISE, hp->sw_advertise);
}

/* Once status is latched (by happy_meal_interrupt) it is cleared by
 * the hardware, so we cannot re-read it and get a correct value.
 *
 * hp->happy_lock must be held
 */
static int happy_meal_is_not_so_happy(struct happy_meal *hp, u32 status)
{
	int reset = 0;

	/* Only print messages for non-counter related interrupts. */
	if (status & (GREG_STAT_STSTERR | GREG_STAT_TFIFO_UND |
		      GREG_STAT_MAXPKTERR | GREG_STAT_RXERR |
		      GREG_STAT_RXPERR | GREG_STAT_RXTERR | GREG_STAT_EOPERR |
		      GREG_STAT_MIFIRQ | GREG_STAT_TXEACK | GREG_STAT_TXLERR |
		      GREG_STAT_TXPERR | GREG_STAT_TXTERR | GREG_STAT_SLVERR |
		      GREG_STAT_SLVPERR))
		netdev_err(hp->dev,
			   "Error interrupt for happy meal, status = %08x\n",
			   status);

	if (status & GREG_STAT_RFIFOVF) {
		/* Receive FIFO overflow is harmless and the hardware will take
		   care of it, just some packets are lost. Who cares. */
		netdev_dbg(hp->dev, "Happy Meal receive FIFO overflow.\n");
	}

	if (status & GREG_STAT_STSTERR) {
		/* BigMAC SQE link test failed. */
		netdev_err(hp->dev, "Happy Meal BigMAC SQE test failed.\n");
		reset = 1;
	}

	if (status & GREG_STAT_TFIFO_UND) {
		/* Transmit FIFO underrun, again DMA error likely. */
		netdev_err(hp->dev,
			   "Happy Meal transmitter FIFO underrun, DMA error.\n");
		reset = 1;
	}

	if (status & GREG_STAT_MAXPKTERR) {
		/* Driver error, tried to transmit something larger
		 * than ethernet max mtu.
		 */
		netdev_err(hp->dev, "Happy Meal MAX Packet size error.\n");
		reset = 1;
	}

	if (status & GREG_STAT_NORXD) {
		/* This is harmless, it just means the system is
		 * quite loaded and the incoming packet rate was
		 * faster than the interrupt handler could keep up
		 * with.
		 */
		netdev_info(hp->dev,
			    "Happy Meal out of receive descriptors, packet dropped.\n");
	}

	if (status & (GREG_STAT_RXERR|GREG_STAT_RXPERR|GREG_STAT_RXTERR)) {
		/* All sorts of DMA receive errors. */
		netdev_err(hp->dev, "Happy Meal rx DMA errors [ %s%s%s]\n",
			   status & GREG_STAT_RXERR ? "GenericError " : "",
			   status & GREG_STAT_RXPERR ? "ParityError " : "",
			   status & GREG_STAT_RXTERR ? "RxTagBotch " : "");
		reset = 1;
	}

	if (status & GREG_STAT_EOPERR) {
		/* Driver bug, didn't set EOP bit in tx descriptor given
		 * to the happy meal.
		 */
		netdev_err(hp->dev,
			   "EOP not set in happy meal transmit descriptor!\n");
		reset = 1;
	}

	if (status & GREG_STAT_MIFIRQ) {
		/* MIF signalled an interrupt, were we polling it? */
		netdev_err(hp->dev, "Happy Meal MIF interrupt.\n");
	}

	if (status &
	    (GREG_STAT_TXEACK|GREG_STAT_TXLERR|GREG_STAT_TXPERR|GREG_STAT_TXTERR)) {
		/* All sorts of transmit DMA errors. */
		netdev_err(hp->dev, "Happy Meal tx DMA errors [ %s%s%s%s]\n",
			   status & GREG_STAT_TXEACK ? "GenericError " : "",
			   status & GREG_STAT_TXLERR ? "LateError " : "",
			   status & GREG_STAT_TXPERR ? "ParityError " : "",
			   status & GREG_STAT_TXTERR ? "TagBotch " : "");
		reset = 1;
	}

	if (status & (GREG_STAT_SLVERR|GREG_STAT_SLVPERR)) {
		/* Bus or parity error when cpu accessed happy meal registers
		 * or it's internal FIFO's.  Should never see this.
		 */
		netdev_err(hp->dev,
			   "Happy Meal register access SBUS slave (%s) error.\n",
			   (status & GREG_STAT_SLVPERR) ? "parity" : "generic");
		reset = 1;
	}

	if (reset) {
		netdev_notice(hp->dev, "Resetting...\n");
		happy_meal_init(hp);
		return 1;
	}
	return 0;
}

/* hp->happy_lock must be held */
static void happy_meal_tx(struct happy_meal *hp)
{
	struct happy_meal_txd *txbase = &hp->happy_block->happy_meal_txd[0];
	struct happy_meal_txd *this;
	struct net_device *dev = hp->dev;
	int elem;

	elem = hp->tx_old;
	while (elem != hp->tx_new) {
		struct sk_buff *skb;
		u32 flags, dma_addr, dma_len;
		int frag;

		netdev_vdbg(hp->dev, "TX[%d]\n", elem);
		this = &txbase[elem];
		flags = hme_read_desc32(hp, &this->tx_flags);
		if (flags & TXFLAG_OWN)
			break;
		skb = hp->tx_skbs[elem];
		if (skb_shinfo(skb)->nr_frags) {
			int last;

			last = elem + skb_shinfo(skb)->nr_frags;
			last &= (TX_RING_SIZE - 1);
			flags = hme_read_desc32(hp, &txbase[last].tx_flags);
			if (flags & TXFLAG_OWN)
				break;
		}
		hp->tx_skbs[elem] = NULL;
		dev->stats.tx_bytes += skb->len;

		for (frag = 0; frag <= skb_shinfo(skb)->nr_frags; frag++) {
			dma_addr = hme_read_desc32(hp, &this->tx_addr);
			dma_len = hme_read_desc32(hp, &this->tx_flags);

			dma_len &= TXFLAG_SIZE;
			if (!frag)
				dma_unmap_single(hp->dma_dev, dma_addr, dma_len, DMA_TO_DEVICE);
			else
				dma_unmap_page(hp->dma_dev, dma_addr, dma_len, DMA_TO_DEVICE);

			elem = NEXT_TX(elem);
			this = &txbase[elem];
		}

		dev_consume_skb_irq(skb);
		dev->stats.tx_packets++;
	}
	hp->tx_old = elem;

	if (netif_queue_stopped(dev) &&
	    TX_BUFFS_AVAIL(hp) > (MAX_SKB_FRAGS + 1))
		netif_wake_queue(dev);
}

/* Originally I used to handle the allocation failure by just giving back just
 * that one ring buffer to the happy meal.  Problem is that usually when that
 * condition is triggered, the happy meal expects you to do something reasonable
 * with all of the packets it has DMA'd in.  So now I just drop the entire
 * ring when we cannot get a new skb and give them all back to the happy meal,
 * maybe things will be "happier" now.
 *
 * hp->happy_lock must be held
 */
static void happy_meal_rx(struct happy_meal *hp, struct net_device *dev)
{
	struct happy_meal_rxd *rxbase = &hp->happy_block->happy_meal_rxd[0];
	struct happy_meal_rxd *this;
	int elem = hp->rx_new, drops = 0;
	u32 flags;

	this = &rxbase[elem];
	while (!((flags = hme_read_desc32(hp, &this->rx_flags)) & RXFLAG_OWN)) {
		struct sk_buff *skb;
		int len = flags >> 16;
		u16 csum = flags & RXFLAG_CSUM;
		u32 dma_addr = hme_read_desc32(hp, &this->rx_addr);

		/* Check for errors. */
		if ((len < ETH_ZLEN) || (flags & RXFLAG_OVERFLOW)) {
			netdev_vdbg(dev, "RX[%d ERR(%08x)]", elem, flags);
			dev->stats.rx_errors++;
			if (len < ETH_ZLEN)
				dev->stats.rx_length_errors++;
			if (len & (RXFLAG_OVERFLOW >> 16)) {
				dev->stats.rx_over_errors++;
				dev->stats.rx_fifo_errors++;
			}

			/* Return it to the Happy meal. */
	drop_it:
			dev->stats.rx_dropped++;
			hme_write_rxd(hp, this,
				      (RXFLAG_OWN|((RX_BUF_ALLOC_SIZE-RX_OFFSET)<<16)),
				      dma_addr);
			goto next;
		}
		skb = hp->rx_skbs[elem];
		if (len > RX_COPY_THRESHOLD) {
			struct sk_buff *new_skb;
			u32 mapping;

			/* Now refill the entry, if we can. */
			new_skb = happy_meal_alloc_skb(RX_BUF_ALLOC_SIZE, GFP_ATOMIC);
			if (new_skb == NULL) {
				drops++;
				goto drop_it;
			}
			skb_put(new_skb, (ETH_FRAME_LEN + RX_OFFSET + 4));
			mapping = dma_map_single(hp->dma_dev, new_skb->data,
						 RX_BUF_ALLOC_SIZE,
						 DMA_FROM_DEVICE);
			if (unlikely(dma_mapping_error(hp->dma_dev, mapping))) {
				dev_kfree_skb_any(new_skb);
				drops++;
				goto drop_it;
			}

			dma_unmap_single(hp->dma_dev, dma_addr, RX_BUF_ALLOC_SIZE, DMA_FROM_DEVICE);
			hp->rx_skbs[elem] = new_skb;
			hme_write_rxd(hp, this,
				      (RXFLAG_OWN|((RX_BUF_ALLOC_SIZE-RX_OFFSET)<<16)),
				      mapping);
			skb_reserve(new_skb, RX_OFFSET);

			/* Trim the original skb for the netif. */
			skb_trim(skb, len);
		} else {
			struct sk_buff *copy_skb = netdev_alloc_skb(dev, len + 2);

			if (copy_skb == NULL) {
				drops++;
				goto drop_it;
			}

			skb_reserve(copy_skb, 2);
			skb_put(copy_skb, len);
			dma_sync_single_for_cpu(hp->dma_dev, dma_addr, len + 2, DMA_FROM_DEVICE);
			skb_copy_from_linear_data(skb, copy_skb->data, len);
			dma_sync_single_for_device(hp->dma_dev, dma_addr, len + 2, DMA_FROM_DEVICE);
			/* Reuse original ring buffer. */
			hme_write_rxd(hp, this,
				      (RXFLAG_OWN|((RX_BUF_ALLOC_SIZE-RX_OFFSET)<<16)),
				      dma_addr);

			skb = copy_skb;
		}

		/* This card is _fucking_ hot... */
		skb->csum = csum_unfold(~(__force __sum16)htons(csum));
		skb->ip_summed = CHECKSUM_COMPLETE;

		netdev_vdbg(dev, "RX[%d len=%d csum=%4x]", elem, len, csum);
		skb->protocol = eth_type_trans(skb, dev);
		netif_rx(skb);

		dev->stats.rx_packets++;
		dev->stats.rx_bytes += len;
	next:
		elem = NEXT_RX(elem);
		this = &rxbase[elem];
	}
	hp->rx_new = elem;
	if (drops)
		netdev_info(hp->dev, "Memory squeeze, deferring packet.\n");
}

static irqreturn_t happy_meal_interrupt(int irq, void *dev_id)
{
	struct net_device *dev = dev_id;
	struct happy_meal *hp  = netdev_priv(dev);
	u32 happy_status       = hme_read32(hp, hp->gregs + GREG_STAT);

	HMD("status=%08x\n", happy_status);
	if (!happy_status)
		return IRQ_NONE;

	spin_lock(&hp->happy_lock);

	if (happy_status & GREG_STAT_ERRORS) {
		if (happy_meal_is_not_so_happy(hp, /* un- */ happy_status))
			goto out;
	}

	if (happy_status & GREG_STAT_TXALL)
		happy_meal_tx(hp);

	if (happy_status & GREG_STAT_RXTOHOST)
		happy_meal_rx(hp, dev);

	HMD("done\n");
out:
	spin_unlock(&hp->happy_lock);

	return IRQ_HANDLED;
}

static int happy_meal_open(struct net_device *dev)
{
	struct happy_meal *hp = netdev_priv(dev);
	int res;

	res = request_irq(hp->irq, happy_meal_interrupt, IRQF_SHARED,
			  dev->name, dev);
	if (res) {
		netdev_err(dev, "Can't order irq %d to go.\n", hp->irq);
		return res;
	}

	HMD("to happy_meal_init\n");

	spin_lock_irq(&hp->happy_lock);
	res = happy_meal_init(hp);
	spin_unlock_irq(&hp->happy_lock);

	if (res)
		free_irq(hp->irq, dev);
	return res;
}

static int happy_meal_close(struct net_device *dev)
{
	struct happy_meal *hp = netdev_priv(dev);

	spin_lock_irq(&hp->happy_lock);
	happy_meal_stop(hp, hp->gregs);
	happy_meal_clean_rings(hp);

	/* If auto-negotiation timer is running, kill it. */
	timer_delete(&hp->happy_timer);

	spin_unlock_irq(&hp->happy_lock);

	free_irq(hp->irq, dev);

	return 0;
}

static void happy_meal_tx_timeout(struct net_device *dev, unsigned int txqueue)
{
	struct happy_meal *hp = netdev_priv(dev);

	netdev_err(dev, "transmit timed out, resetting\n");
	tx_dump_log();
	netdev_err(dev, "Happy Status %08x TX[%08x:%08x]\n",
		   hme_read32(hp, hp->gregs + GREG_STAT),
		   hme_read32(hp, hp->etxregs + ETX_CFG),
		   hme_read32(hp, hp->bigmacregs + BMAC_TXCFG));

	spin_lock_irq(&hp->happy_lock);
	happy_meal_init(hp);
	spin_unlock_irq(&hp->happy_lock);

	netif_wake_queue(dev);
}

static void unmap_partial_tx_skb(struct happy_meal *hp, u32 first_mapping,
				 u32 first_len, u32 first_entry, u32 entry)
{
	struct happy_meal_txd *txbase = &hp->happy_block->happy_meal_txd[0];

	dma_unmap_single(hp->dma_dev, first_mapping, first_len, DMA_TO_DEVICE);

	first_entry = NEXT_TX(first_entry);
	while (first_entry != entry) {
		struct happy_meal_txd *this = &txbase[first_entry];
		u32 addr, len;

		addr = hme_read_desc32(hp, &this->tx_addr);
		len = hme_read_desc32(hp, &this->tx_flags);
		len &= TXFLAG_SIZE;
		dma_unmap_page(hp->dma_dev, addr, len, DMA_TO_DEVICE);
	}
}

static netdev_tx_t happy_meal_start_xmit(struct sk_buff *skb,
					 struct net_device *dev)
{
	struct happy_meal *hp = netdev_priv(dev);
	int entry;
	u32 tx_flags;

	tx_flags = TXFLAG_OWN;
	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		const u32 csum_start_off = skb_checksum_start_offset(skb);
		const u32 csum_stuff_off = csum_start_off + skb->csum_offset;

		tx_flags = (TXFLAG_OWN | TXFLAG_CSENABLE |
			    ((csum_start_off << 14) & TXFLAG_CSBUFBEGIN) |
			    ((csum_stuff_off << 20) & TXFLAG_CSLOCATION));
	}

	spin_lock_irq(&hp->happy_lock);

	if (TX_BUFFS_AVAIL(hp) <= (skb_shinfo(skb)->nr_frags + 1)) {
		netif_stop_queue(dev);
		spin_unlock_irq(&hp->happy_lock);
		netdev_err(dev, "BUG! Tx Ring full when queue awake!\n");
		return NETDEV_TX_BUSY;
	}

	entry = hp->tx_new;
	netdev_vdbg(dev, "SX<l[%d]e[%d]>\n", skb->len, entry);
	hp->tx_skbs[entry] = skb;

	if (skb_shinfo(skb)->nr_frags == 0) {
		u32 mapping, len;

		len = skb->len;
		mapping = dma_map_single(hp->dma_dev, skb->data, len, DMA_TO_DEVICE);
		if (unlikely(dma_mapping_error(hp->dma_dev, mapping)))
			goto out_dma_error;
		tx_flags |= (TXFLAG_SOP | TXFLAG_EOP);
		hme_write_txd(hp, &hp->happy_block->happy_meal_txd[entry],
			      (tx_flags | (len & TXFLAG_SIZE)),
			      mapping);
		entry = NEXT_TX(entry);
	} else {
		u32 first_len, first_mapping;
		int frag, first_entry = entry;

		/* We must give this initial chunk to the device last.
		 * Otherwise we could race with the device.
		 */
		first_len = skb_headlen(skb);
		first_mapping = dma_map_single(hp->dma_dev, skb->data, first_len,
					       DMA_TO_DEVICE);
		if (unlikely(dma_mapping_error(hp->dma_dev, first_mapping)))
			goto out_dma_error;
		entry = NEXT_TX(entry);

		for (frag = 0; frag < skb_shinfo(skb)->nr_frags; frag++) {
			const skb_frag_t *this_frag = &skb_shinfo(skb)->frags[frag];
			u32 len, mapping, this_txflags;

			len = skb_frag_size(this_frag);
			mapping = skb_frag_dma_map(hp->dma_dev, this_frag,
						   0, len, DMA_TO_DEVICE);
			if (unlikely(dma_mapping_error(hp->dma_dev, mapping))) {
				unmap_partial_tx_skb(hp, first_mapping, first_len,
						     first_entry, entry);
				goto out_dma_error;
			}
			this_txflags = tx_flags;
			if (frag == skb_shinfo(skb)->nr_frags - 1)
				this_txflags |= TXFLAG_EOP;
			hme_write_txd(hp, &hp->happy_block->happy_meal_txd[entry],
				      (this_txflags | (len & TXFLAG_SIZE)),
				      mapping);
			entry = NEXT_TX(entry);
		}
		hme_write_txd(hp, &hp->happy_block->happy_meal_txd[first_entry],
			      (tx_flags | TXFLAG_SOP | (first_len & TXFLAG_SIZE)),
			      first_mapping);
	}

	hp->tx_new = entry;

	if (TX_BUFFS_AVAIL(hp) <= (MAX_SKB_FRAGS + 1))
		netif_stop_queue(dev);

	/* Get it going. */
	hme_write32(hp, hp->etxregs + ETX_PENDING, ETX_TP_DMAWAKEUP);

	spin_unlock_irq(&hp->happy_lock);

	tx_add_log(hp, TXLOG_ACTION_TXMIT, 0);
	return NETDEV_TX_OK;

out_dma_error:
	hp->tx_skbs[hp->tx_new] = NULL;
	spin_unlock_irq(&hp->happy_lock);

	dev_kfree_skb_any(skb);
	dev->stats.tx_dropped++;
	return NETDEV_TX_OK;
}

static struct net_device_stats *happy_meal_get_stats(struct net_device *dev)
{
	struct happy_meal *hp = netdev_priv(dev);

	spin_lock_irq(&hp->happy_lock);
	happy_meal_get_counters(hp, hp->bigmacregs);
	spin_unlock_irq(&hp->happy_lock);

	return &dev->stats;
}

static void happy_meal_set_multicast(struct net_device *dev)
{
	struct happy_meal *hp = netdev_priv(dev);
	void __iomem *bregs = hp->bigmacregs;
	struct netdev_hw_addr *ha;
	u32 crc;

	spin_lock_irq(&hp->happy_lock);

	if ((dev->flags & IFF_ALLMULTI) || (netdev_mc_count(dev) > 64)) {
		hme_write32(hp, bregs + BMAC_HTABLE0, 0xffff);
		hme_write32(hp, bregs + BMAC_HTABLE1, 0xffff);
		hme_write32(hp, bregs + BMAC_HTABLE2, 0xffff);
		hme_write32(hp, bregs + BMAC_HTABLE3, 0xffff);
	} else if (dev->flags & IFF_PROMISC) {
		hme_write32(hp, bregs + BMAC_RXCFG,
			    hme_read32(hp, bregs + BMAC_RXCFG) | BIGMAC_RXCFG_PMISC);
	} else {
		u16 hash_table[4];

		memset(hash_table, 0, sizeof(hash_table));
		netdev_for_each_mc_addr(ha, dev) {
			crc = ether_crc_le(6, ha->addr);
			crc >>= 26;
			hash_table[crc >> 4] |= 1 << (crc & 0xf);
		}
		hme_write32(hp, bregs + BMAC_HTABLE0, hash_table[0]);
		hme_write32(hp, bregs + BMAC_HTABLE1, hash_table[1]);
		hme_write32(hp, bregs + BMAC_HTABLE2, hash_table[2]);
		hme_write32(hp, bregs + BMAC_HTABLE3, hash_table[3]);
	}

	spin_unlock_irq(&hp->happy_lock);
}

/* Ethtool support... */
static int hme_get_link_ksettings(struct net_device *dev,
				  struct ethtool_link_ksettings *cmd)
{
	struct happy_meal *hp = netdev_priv(dev);
	u32 speed;
	u32 supported;

	supported =
		(SUPPORTED_10baseT_Half | SUPPORTED_10baseT_Full |
		 SUPPORTED_100baseT_Half | SUPPORTED_100baseT_Full |
		 SUPPORTED_Autoneg | SUPPORTED_TP | SUPPORTED_MII);

	/* XXX hardcoded stuff for now */
	cmd->base.port = PORT_TP; /* XXX no MII support */
	cmd->base.phy_address = 0; /* XXX fixed PHYAD */

	/* Record PHY settings. */
	spin_lock_irq(&hp->happy_lock);
	hp->sw_bmcr = happy_meal_tcvr_read(hp, hp->tcvregs, MII_BMCR);
	hp->sw_lpa = happy_meal_tcvr_read(hp, hp->tcvregs, MII_LPA);
	spin_unlock_irq(&hp->happy_lock);

	if (hp->sw_bmcr & BMCR_ANENABLE) {
		cmd->base.autoneg = AUTONEG_ENABLE;
		speed = ((hp->sw_lpa & (LPA_100HALF | LPA_100FULL)) ?
			 SPEED_100 : SPEED_10);
		if (speed == SPEED_100)
			cmd->base.duplex =
				(hp->sw_lpa & (LPA_100FULL)) ?
				DUPLEX_FULL : DUPLEX_HALF;
		else
			cmd->base.duplex =
				(hp->sw_lpa & (LPA_10FULL)) ?
				DUPLEX_FULL : DUPLEX_HALF;
	} else {
		cmd->base.autoneg = AUTONEG_DISABLE;
		speed = (hp->sw_bmcr & BMCR_SPEED100) ? SPEED_100 : SPEED_10;
		cmd->base.duplex =
			(hp->sw_bmcr & BMCR_FULLDPLX) ?
			DUPLEX_FULL : DUPLEX_HALF;
	}
	cmd->base.speed = speed;
	ethtool_convert_legacy_u32_to_link_mode(cmd->link_modes.supported,
						supported);

	return 0;
}

static int hme_set_link_ksettings(struct net_device *dev,
				  const struct ethtool_link_ksettings *cmd)
{
	struct happy_meal *hp = netdev_priv(dev);

	/* Verify the settings we care about. */
	if (cmd->base.autoneg != AUTONEG_ENABLE &&
	    cmd->base.autoneg != AUTONEG_DISABLE)
		return -EINVAL;
	if (cmd->base.autoneg == AUTONEG_DISABLE &&
	    ((cmd->base.speed != SPEED_100 &&
	      cmd->base.speed != SPEED_10) ||
	     (cmd->base.duplex != DUPLEX_HALF &&
	      cmd->base.duplex != DUPLEX_FULL)))
		return -EINVAL;

	/* Ok, do it to it. */
	spin_lock_irq(&hp->happy_lock);
	timer_delete(&hp->happy_timer);
	happy_meal_begin_auto_negotiation(hp, hp->tcvregs, cmd);
	spin_unlock_irq(&hp->happy_lock);

	return 0;
}

static void hme_get_drvinfo(struct net_device *dev, struct ethtool_drvinfo *info)
{
	struct happy_meal *hp = netdev_priv(dev);

	strscpy(info->driver, DRV_NAME, sizeof(info->driver));
	if (hp->happy_flags & HFLAG_PCI) {
		struct pci_dev *pdev = hp->happy_dev;
		strscpy(info->bus_info, pci_name(pdev), sizeof(info->bus_info));
	}
#ifdef CONFIG_SBUS
	else {
		const struct linux_prom_registers *regs;
		struct platform_device *op = hp->happy_dev;
		regs = of_get_property(op->dev.of_node, "regs", NULL);
		if (regs)
			snprintf(info->bus_info, sizeof(info->bus_info),
				"SBUS:%d",
				regs->which_io);
	}
#endif
}

static u32 hme_get_link(struct net_device *dev)
{
	struct happy_meal *hp = netdev_priv(dev);

	spin_lock_irq(&hp->happy_lock);
	hp->sw_bmcr = happy_meal_tcvr_read(hp, hp->tcvregs, MII_BMCR);
	spin_unlock_irq(&hp->happy_lock);

	return hp->sw_bmsr & BMSR_LSTATUS;
}

static const struct ethtool_ops hme_ethtool_ops = {
	.get_drvinfo		= hme_get_drvinfo,
	.get_link		= hme_get_link,
	.get_link_ksettings	= hme_get_link_ksettings,
	.set_link_ksettings	= hme_set_link_ksettings,
};

#ifdef CONFIG_SBUS
/* Given a happy meal sbus device, find it's quattro parent.
 * If none exist, allocate and return a new one.
 *
 * Return NULL on failure.
 */
static struct quattro *quattro_sbus_find(struct platform_device *child)
{
	struct device *parent = child->dev.parent;
	struct platform_device *op;
	struct quattro *qp;

	op = to_platform_device(parent);
	qp = platform_get_drvdata(op);
	if (qp)
		return qp;

	qp = kzalloc(sizeof(*qp), GFP_KERNEL);
	if (!qp)
		return NULL;

	qp->quattro_dev = child;
	qp->next = qfe_sbus_list;
	qfe_sbus_list = qp;

	platform_set_drvdata(op, qp);
	return qp;
}
#endif /* CONFIG_SBUS */

#ifdef CONFIG_PCI
static struct quattro *quattro_pci_find(struct pci_dev *pdev)
{
	int i;
	struct pci_dev *bdev = pdev->bus->self;
	struct quattro *qp;

	if (!bdev)
		return ERR_PTR(-ENODEV);

	for (qp = qfe_pci_list; qp != NULL; qp = qp->next) {
		struct pci_dev *qpdev = qp->quattro_dev;

		if (qpdev == bdev)
			return qp;
	}

	qp = kmalloc(sizeof(struct quattro), GFP_KERNEL);
	if (!qp)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < 4; i++)
		qp->happy_meals[i] = NULL;

	qp->quattro_dev = bdev;
	qp->next = qfe_pci_list;
	qfe_pci_list = qp;

	/* No range tricks necessary on PCI. */
	qp->nranges = 0;
	return qp;
}
#endif /* CONFIG_PCI */

static const struct net_device_ops hme_netdev_ops = {
	.ndo_open		= happy_meal_open,
	.ndo_stop		= happy_meal_close,
	.ndo_start_xmit		= happy_meal_start_xmit,
	.ndo_tx_timeout		= happy_meal_tx_timeout,
	.ndo_get_stats		= happy_meal_get_stats,
	.ndo_set_rx_mode	= happy_meal_set_multicast,
	.ndo_set_mac_address 	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};

#ifdef CONFIG_PCI
static int is_quattro_p(struct pci_dev *pdev)
{
	struct pci_dev *busdev = pdev->bus->self;
	struct pci_dev *this_pdev;
	int n_hmes;

	if (!busdev || busdev->vendor != PCI_VENDOR_ID_DEC ||
	    busdev->device != PCI_DEVICE_ID_DEC_21153)
		return 0;

	n_hmes = 0;
	list_for_each_entry(this_pdev, &pdev->bus->devices, bus_list) {
		if (this_pdev->vendor == PCI_VENDOR_ID_SUN &&
		    this_pdev->device == PCI_DEVICE_ID_SUN_HAPPYMEAL)
			n_hmes++;
	}

	if (n_hmes != 4)
		return 0;

	return 1;
}

/* Fetch MAC address from vital product data of PCI ROM. */
static int find_eth_addr_in_vpd(void __iomem *rom_base, int len, int index, unsigned char *dev_addr)
{
	int this_offset;

	for (this_offset = 0x20; this_offset < len; this_offset++) {
		void __iomem *p = rom_base + this_offset;

		if (readb(p + 0) != 0x90 ||
		    readb(p + 1) != 0x00 ||
		    readb(p + 2) != 0x09 ||
		    readb(p + 3) != 0x4e ||
		    readb(p + 4) != 0x41 ||
		    readb(p + 5) != 0x06)
			continue;

		this_offset += 6;
		p += 6;

		if (index == 0) {
			for (int i = 0; i < 6; i++)
				dev_addr[i] = readb(p + i);
			return 1;
		}
		index--;
	}
	return 0;
}

static void __maybe_unused get_hme_mac_nonsparc(struct pci_dev *pdev,
						unsigned char *dev_addr)
{
	void __iomem *p;
	size_t size;

	p = pci_map_rom(pdev, &size);
	if (p) {
		int index = 0;
		int found;

		if (is_quattro_p(pdev))
			index = PCI_SLOT(pdev->devfn);

		found = readb(p) == 0x55 &&
			readb(p + 1) == 0xaa &&
			find_eth_addr_in_vpd(p, (64 * 1024), index, dev_addr);
		pci_unmap_rom(pdev, p);
		if (found)
			return;
	}

	/* Sun MAC prefix then 3 random bytes. */
	dev_addr[0] = 0x08;
	dev_addr[1] = 0x00;
	dev_addr[2] = 0x20;
	get_random_bytes(&dev_addr[3], 3);
}
#endif

static void happy_meal_addr_init(struct happy_meal *hp,
				 struct device_node *dp, int qfe_slot)
{
	int i;

	for (i = 0; i < 6; i++) {
		if (macaddr[i] != 0)
			break;
	}

	if (i < 6) { /* a mac address was given */
		u8 addr[ETH_ALEN];

		for (i = 0; i < 6; i++)
			addr[i] = macaddr[i];
		eth_hw_addr_set(hp->dev, addr);
		macaddr[5]++;
	} else {
#ifdef CONFIG_SPARC
		const unsigned char *addr;
		int len;

		/* If user did not specify a MAC address specifically, use
		 * the Quattro local-mac-address property...
		 */
		if (qfe_slot != -1) {
			addr = of_get_property(dp, "local-mac-address", &len);
			if (addr && len == 6) {
				eth_hw_addr_set(hp->dev, addr);
				return;
			}
		}

		eth_hw_addr_set(hp->dev, idprom->id_ethaddr);
#else
		u8 addr[ETH_ALEN];

		get_hme_mac_nonsparc(hp->happy_dev, addr);
		eth_hw_addr_set(hp->dev, addr);
#endif
	}
}

static int happy_meal_common_probe(struct happy_meal *hp,
				   struct device_node *dp)
{
	struct net_device *dev = hp->dev;
	int err;

#ifdef CONFIG_SPARC
	hp->hm_revision = of_getintprop_default(dp, "hm-rev", hp->hm_revision);
#endif

	/* Now enable the feature flags we can. */
	if (hp->hm_revision == 0x20 || hp->hm_revision == 0x21)
		hp->happy_flags |= HFLAG_20_21;
	else if (hp->hm_revision != 0xa0)
		hp->happy_flags |= HFLAG_NOT_A0;

	hp->happy_block = dmam_alloc_coherent(hp->dma_dev, PAGE_SIZE,
					      &hp->hblock_dvma, GFP_KERNEL);
	if (!hp->happy_block)
		return -ENOMEM;

	/* Force check of the link first time we are brought up. */
	hp->linkcheck = 0;

	/* Force timer state to 'asleep' with count of zero. */
	hp->timer_state = asleep;
	hp->timer_ticks = 0;

	timer_setup(&hp->happy_timer, happy_meal_timer, 0);

	dev->netdev_ops = &hme_netdev_ops;
	dev->watchdog_timeo = 5 * HZ;
	dev->ethtool_ops = &hme_ethtool_ops;

	/* Happy Meal can do it all... */
	dev->hw_features = NETIF_F_SG | NETIF_F_HW_CSUM;
	dev->features |= dev->hw_features | NETIF_F_RXCSUM;


	/* Grrr, Happy Meal comes up by default not advertising
	 * full duplex 100baseT capabilities, fix this.
	 */
	spin_lock_irq(&hp->happy_lock);
	happy_meal_set_initial_advertisement(hp);
	spin_unlock_irq(&hp->happy_lock);

	err = devm_register_netdev(hp->dma_dev, dev);
	if (err)
		dev_err(hp->dma_dev, "Cannot register net device, aborting.\n");
	return err;
}

#ifdef CONFIG_SBUS
static int happy_meal_sbus_probe_one(struct platform_device *op, int is_qfe)
{
	struct device_node *dp = op->dev.of_node, *sbus_dp;
	struct quattro *qp = NULL;
	struct happy_meal *hp;
	struct net_device *dev;
	int qfe_slot = -1;
	int err;

	sbus_dp = op->dev.parent->of_node;

	/* We can match PCI devices too, do not accept those here. */
	if (!of_node_name_eq(sbus_dp, "sbus") && !of_node_name_eq(sbus_dp, "sbi"))
		return -ENODEV;

	if (is_qfe) {
		qp = quattro_sbus_find(op);
		if (qp == NULL)
			return -ENODEV;
		for (qfe_slot = 0; qfe_slot < 4; qfe_slot++)
			if (qp->happy_meals[qfe_slot] == NULL)
				break;
		if (qfe_slot == 4)
			return -ENODEV;
	}

	dev = devm_alloc_etherdev(&op->dev, sizeof(struct happy_meal));
	if (!dev)
		return -ENOMEM;
	SET_NETDEV_DEV(dev, &op->dev);

	hp = netdev_priv(dev);
	hp->dev = dev;
	hp->happy_dev = op;
	hp->dma_dev = &op->dev;
	happy_meal_addr_init(hp, dp, qfe_slot);

	spin_lock_init(&hp->happy_lock);

	if (qp != NULL) {
		hp->qfe_parent = qp;
		hp->qfe_ent = qfe_slot;
		qp->happy_meals[qfe_slot] = dev;
	}

	hp->gregs = devm_platform_ioremap_resource(op, 0);
	if (IS_ERR(hp->gregs)) {
		dev_err(&op->dev, "Cannot map global registers.\n");
		err = PTR_ERR(hp->gregs);
		goto err_out_clear_quattro;
	}

	hp->etxregs = devm_platform_ioremap_resource(op, 1);
	if (IS_ERR(hp->etxregs)) {
		dev_err(&op->dev, "Cannot map MAC TX registers.\n");
		err = PTR_ERR(hp->etxregs);
		goto err_out_clear_quattro;
	}

	hp->erxregs = devm_platform_ioremap_resource(op, 2);
	if (IS_ERR(hp->erxregs)) {
		dev_err(&op->dev, "Cannot map MAC RX registers.\n");
		err = PTR_ERR(hp->erxregs);
		goto err_out_clear_quattro;
	}

	hp->bigmacregs = devm_platform_ioremap_resource(op, 3);
	if (IS_ERR(hp->bigmacregs)) {
		dev_err(&op->dev, "Cannot map BIGMAC registers.\n");
		err = PTR_ERR(hp->bigmacregs);
		goto err_out_clear_quattro;
	}

	hp->tcvregs = devm_platform_ioremap_resource(op, 4);
	if (IS_ERR(hp->tcvregs)) {
		dev_err(&op->dev, "Cannot map TCVR registers.\n");
		err = PTR_ERR(hp->tcvregs);
		goto err_out_clear_quattro;
	}

	hp->hm_revision = 0xa0;

	if (qp != NULL)
		hp->happy_flags |= HFLAG_QUATTRO;

	hp->irq = op->archdata.irqs[0];

	/* Get the supported DVMA burst sizes from our Happy SBUS. */
	hp->happy_bursts = of_getintprop_default(sbus_dp,
						 "burst-sizes", 0x00);

#ifdef CONFIG_PCI
	/* Hook up SBUS register/descriptor accessors. */
	hp->read_desc32 = sbus_hme_read_desc32;
	hp->write_txd = sbus_hme_write_txd;
	hp->write_rxd = sbus_hme_write_rxd;
	hp->read32 = sbus_hme_read32;
	hp->write32 = sbus_hme_write32;
#endif

	err = happy_meal_common_probe(hp, dp);
	if (err)
		goto err_out_clear_quattro;

	platform_set_drvdata(op, hp);

	if (qfe_slot != -1)
		netdev_info(dev,
			    "Quattro HME slot %d (SBUS) 10/100baseT Ethernet %pM\n",
			    qfe_slot, dev->dev_addr);
	else
		netdev_info(dev, "HAPPY MEAL (SBUS) 10/100baseT Ethernet %pM\n",
			    dev->dev_addr);

	return 0;

err_out_clear_quattro:
	if (qp)
		qp->happy_meals[qfe_slot] = NULL;
	return err;
}
#endif

#ifdef CONFIG_PCI
static int happy_meal_pci_probe(struct pci_dev *pdev,
				const struct pci_device_id *ent)
{
	struct device_node *dp = NULL;
	struct quattro *qp = NULL;
	struct happy_meal *hp;
	struct net_device *dev;
	void __iomem *hpreg_base;
	struct resource *hpreg_res;
	char prom_name[64];
	int qfe_slot = -1;
	int err = -ENODEV;

	/* Now make sure pci_dev cookie is there. */
#ifdef CONFIG_SPARC
	dp = pci_device_to_OF_node(pdev);
	snprintf(prom_name, sizeof(prom_name), "%pOFn", dp);
#else
	if (is_quattro_p(pdev))
		strcpy(prom_name, "SUNW,qfe");
	else
		strcpy(prom_name, "SUNW,hme");
#endif

	err = pcim_enable_device(pdev);
	if (err)
		return err;
	pci_set_master(pdev);

	if (!strcmp(prom_name, "SUNW,qfe") || !strcmp(prom_name, "qfe")) {
		qp = quattro_pci_find(pdev);
		if (IS_ERR(qp))
			return PTR_ERR(qp);

		for (qfe_slot = 0; qfe_slot < 4; qfe_slot++)
			if (!qp->happy_meals[qfe_slot])
				break;

		if (qfe_slot == 4)
			return -ENODEV;
	}

	dev = devm_alloc_etherdev(&pdev->dev, sizeof(struct happy_meal));
	if (!dev)
		return -ENOMEM;
	SET_NETDEV_DEV(dev, &pdev->dev);

	hp = netdev_priv(dev);
	hp->dev = dev;
	hp->happy_dev = pdev;
	hp->dma_dev = &pdev->dev;

	spin_lock_init(&hp->happy_lock);

	if (qp != NULL) {
		hp->qfe_parent = qp;
		hp->qfe_ent = qfe_slot;
		qp->happy_meals[qfe_slot] = dev;
	}

	err = -EINVAL;
	if ((pci_resource_flags(pdev, 0) & IORESOURCE_IO) != 0) {
		dev_err(&pdev->dev,
			"Cannot find proper PCI device base address.\n");
		goto err_out_clear_quattro;
	}

	hpreg_res = devm_request_mem_region(&pdev->dev,
					    pci_resource_start(pdev, 0),
					    pci_resource_len(pdev, 0),
					    DRV_NAME);
	if (!hpreg_res) {
		err = -EBUSY;
		dev_err(&pdev->dev, "Cannot obtain PCI resources, aborting.\n");
		goto err_out_clear_quattro;
	}

	hpreg_base = pcim_iomap(pdev, 0, 0x8000);
	if (!hpreg_base) {
		err = -ENOMEM;
		dev_err(&pdev->dev, "Unable to remap card memory.\n");
		goto err_out_clear_quattro;
	}

	happy_meal_addr_init(hp, dp, qfe_slot);

	/* Layout registers. */
	hp->gregs      = (hpreg_base + 0x0000UL);
	hp->etxregs    = (hpreg_base + 0x2000UL);
	hp->erxregs    = (hpreg_base + 0x4000UL);
	hp->bigmacregs = (hpreg_base + 0x6000UL);
	hp->tcvregs    = (hpreg_base + 0x7000UL);

	if (IS_ENABLED(CONFIG_SPARC))
		hp->hm_revision = 0xc0 | (pdev->revision & 0x0f);
	else
		hp->hm_revision = 0x20;

	if (qp != NULL)
		hp->happy_flags |= HFLAG_QUATTRO;

	/* And of course, indicate this is PCI. */
	hp->happy_flags |= HFLAG_PCI;

#ifdef CONFIG_SPARC
	/* Assume PCI happy meals can handle all burst sizes. */
	hp->happy_bursts = DMA_BURSTBITS;
#endif
	hp->irq = pdev->irq;

#ifdef CONFIG_SBUS
	/* Hook up PCI register/descriptor accessors. */
	hp->read_desc32 = pci_hme_read_desc32;
	hp->write_txd = pci_hme_write_txd;
	hp->write_rxd = pci_hme_write_rxd;
	hp->read32 = pci_hme_read32;
	hp->write32 = pci_hme_write32;
#endif

	err = happy_meal_common_probe(hp, dp);
	if (err)
		goto err_out_clear_quattro;

	pci_set_drvdata(pdev, hp);

	if (!qfe_slot) {
		struct pci_dev *qpdev = qp->quattro_dev;

		prom_name[0] = 0;
		if (!strncmp(dev->name, "eth", 3)) {
			int i = simple_strtoul(dev->name + 3, NULL, 10);
			sprintf(prom_name, "-%d", i + 3);
		}
		netdev_info(dev,
			    "%s: Quattro HME (PCI/CheerIO) 10/100baseT Ethernet bridge %04x.%04x\n",
			    prom_name, qpdev->vendor, qpdev->device);
	}

	if (qfe_slot != -1)
		netdev_info(dev,
			    "Quattro HME slot %d (PCI/CheerIO) 10/100baseT Ethernet %pM\n",
			    qfe_slot, dev->dev_addr);
	else
		netdev_info(dev,
			    "HAPPY MEAL (PCI/CheerIO) 10/100BaseT Ethernet %pM\n",
			    dev->dev_addr);

	return 0;

err_out_clear_quattro:
	if (qp != NULL)
		qp->happy_meals[qfe_slot] = NULL;
	return err;
}

static const struct pci_device_id happymeal_pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_SUN, PCI_DEVICE_ID_SUN_HAPPYMEAL) },
	{ }			/* Terminating entry */
};

MODULE_DEVICE_TABLE(pci, happymeal_pci_ids);

static struct pci_driver hme_pci_driver = {
	.name		= "hme",
	.id_table	= happymeal_pci_ids,
	.probe		= happy_meal_pci_probe,
};

static int __init happy_meal_pci_init(void)
{
	return pci_register_driver(&hme_pci_driver);
}

static void happy_meal_pci_exit(void)
{
	pci_unregister_driver(&hme_pci_driver);

	while (qfe_pci_list) {
		struct quattro *qfe = qfe_pci_list;
		struct quattro *next = qfe->next;

		kfree(qfe);

		qfe_pci_list = next;
	}
}

#endif

#ifdef CONFIG_SBUS
static const struct of_device_id hme_sbus_match[];
static int hme_sbus_probe(struct platform_device *op)
{
	const struct of_device_id *match;
	struct device_node *dp = op->dev.of_node;
	const char *model = of_get_property(dp, "model", NULL);
	int is_qfe;

	match = of_match_device(hme_sbus_match, &op->dev);
	if (!match)
		return -EINVAL;
	is_qfe = (match->data != NULL);

	if (!is_qfe && model && !strcmp(model, "SUNW,sbus-qfe"))
		is_qfe = 1;

	return happy_meal_sbus_probe_one(op, is_qfe);
}

static const struct of_device_id hme_sbus_match[] = {
	{
		.name = "SUNW,hme",
	},
	{
		.name = "SUNW,qfe",
		.data = (void *) 1,
	},
	{
		.name = "qfe",
		.data = (void *) 1,
	},
	{},
};

MODULE_DEVICE_TABLE(of, hme_sbus_match);

static struct platform_driver hme_sbus_driver = {
	.driver = {
		.name = "hme",
		.of_match_table = hme_sbus_match,
	},
	.probe		= hme_sbus_probe,
};

static int __init happy_meal_sbus_init(void)
{
	return platform_driver_register(&hme_sbus_driver);
}

static void happy_meal_sbus_exit(void)
{
	platform_driver_unregister(&hme_sbus_driver);

	while (qfe_sbus_list) {
		struct quattro *qfe = qfe_sbus_list;
		struct quattro *next = qfe->next;

		kfree(qfe);

		qfe_sbus_list = next;
	}
}
#endif

static int __init happy_meal_probe(void)
{
	int err = 0;

#ifdef CONFIG_SBUS
	err = happy_meal_sbus_init();
#endif
#ifdef CONFIG_PCI
	if (!err) {
		err = happy_meal_pci_init();
#ifdef CONFIG_SBUS
		if (err)
			happy_meal_sbus_exit();
#endif
	}
#endif

	return err;
}


static void __exit happy_meal_exit(void)
{
#ifdef CONFIG_SBUS
	happy_meal_sbus_exit();
#endif
#ifdef CONFIG_PCI
	happy_meal_pci_exit();
#endif
}

module_init(happy_meal_probe);
module_exit(happy_meal_exit);
