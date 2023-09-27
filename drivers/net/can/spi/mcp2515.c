/*
 * mcp2515.c: driver for Microchip MCP2515 SPI CAN controller
 *
 * Copyright (c) 2010 Andre B. Oliveira <anbadeol@gmail.com>
 * Copyright (c) 2012 Marc Kleine-Budde <mkl@pengutronix.de>, Pengutronix
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * References: Microchip MCP2515 data sheet, DS21801E, 2007.
 */

#include <linux/can.h>
#include <linux/can/dev.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/skbuff.h>
#include <linux/spi/spi.h>
#include <linux/spinlock.h>
#include <linux/timer.h>

MODULE_DESCRIPTION("Driver for Microchip MCP2515 SPI CAN controller");
MODULE_AUTHOR("Andre B. Oliveira <anbadeol@gmail.com>, "
	      "Marc Kleine-Budde <mkl@pengutronix.de>");
MODULE_LICENSE("GPL");

/* SPI interface instruction set */
#define MCP2515_INSTRUCTION_WRITE	0x02
#define MCP2515_INSTRUCTION_READ	0x03
#define MCP2515_INSTRUCTION_BIT_MODIFY	0x05
#define MCP2515_INSTRUCTION_LOAD_TXB(n)	(0x40 + ((n) << 1))
#define MCP2515_INSTRUCTION_RTS(n)	(0x80 + (1 << (n)))
#define MCP2515_INSTRUCTION_READ_RXB(n)	(0x90 + ((n) << 2))
#define MCP2515_INSTRUCTION_RESET	0xc0

/* Registers */
#define CANSTAT				0x0e
#define CANCTRL				0x0f
#define TEC				0x1c
#define REC				0x1d
#define CANINTF				0x2c
#define EFLAG				0x2d
#define CNF3				0x28
#define RXB0CTRL			0x60
#define RXB1CTRL			0x70

/* CANCTRL bits */
#define CANCTRL_REQOP_NORMAL		0x00
#define CANCTRL_REQOP_SLEEP		0x20
#define CANCTRL_REQOP_LOOPBACK		0x40
#define CANCTRL_REQOP_LISTEN_ONLY	0x60
#define CANCTRL_REQOP_CONF		0x80
#define CANCTRL_REQOP_MASK		0xe0
#define CANCTRL_OSM			BIT(3)
#define CANCTRL_ABAT			BIT(4)

/* CANINTF bits */
#define CANINTF_RX0IF			BIT(0)
#define CANINTF_RX1IF			BIT(1)
#define CANINTF_TX0IF			BIT(2)
#define CANINTF_TX1IF			BIT(3)
#define CANINTF_TX2IF			BIT(4)
#define CANINTF_ERRIF			BIT(5)
#define CANINTF_WAKIF			BIT(6)
#define CANINTF_MERRF			BIT(7)

/* EFLG bits */
#define EFLG_RX0OVR			BIT(6)
#define EFLG_RX1OVR			BIT(7)

/* CNF2 bits */
#define CNF2_BTLMODE			BIT(7)
#define CNF2_SAM			BIT(6)

/* CANINTE bits */
#define CANINTE_RX0IE			BIT(0)
#define CANINTE_RX1IE			BIT(1)
#define CANINTE_TX0IE			BIT(2)
#define CANINTE_TX1IE			BIT(3)
#define CANINTE_TX2IE			BIT(4)
#define CANINTE_ERRIE			BIT(5)
#define CANINTE_WAKIE			BIT(6)
#define CANINTE_MERRE			BIT(7)
#define CANINTE_RX			(CANINTE_RX0IE | CANINTE_RX1IE)
#define CANINTE_TX \
	(CANINTE_TX0IE | CANINTE_TX1IE | CANINTE_TX2IE)
#define CANINTE_ERR			(CANINTE_ERRIE)

/* RXBnCTRL bits */
#define RXBCTRL_BUKT			BIT(2)
#define RXBCTRL_RXM0			BIT(5)
#define RXBCTRL_RXM1			BIT(6)

/* RXBnSIDL bits */
#define RXBSIDL_IDE			BIT(3)
#define RXBSIDL_SRR			BIT(4)

/* RXBnDLC bits */
#define RXBDLC_RTR			BIT(6)

#define MCP2515_DMA_SIZE		32
#define MCP2515_IRQ_DELAY		(HZ / 5)
#define MCP2515_TX_CNT 			3

#define TX_MAP_BUSY	(1 << MCP2515_TX_CNT) - 1

#define READ_FLAGS_TIMER_VAL_MS (msecs_to_jiffies(200))

/* Network device private data */
struct mcp2515_priv {
	struct can_priv can;		/* must be first for all CAN network devices */
	struct spi_device *spi;		/* SPI device */
	struct clk *clk;		/* External clock (usu. an oscillator) */
	struct regulator *power;	/* Chip power regulator (optional) */
	struct regulator *transceiver;	/* Transceiver power regulator (optional) */

	u8 canintf;		/* last read value of CANINTF register */
	u8 eflg;		/* last read value of EFLG register */

	struct sk_buff *skb[MCP2515_TX_CNT];

	spinlock_t lock;	/* Lock for the following flags: */
	unsigned busy:1;	/* set when pending async spi transaction */
	unsigned interrupt:1;	/* set when pending interrupt handling */
	unsigned netif_queue_stopped:1;
	u8 loaded_txb;		/* set to the currently loaded transmit buffer */
	u8 tx_busy_map;		/* set for busy transmit buffer(s) */
	u8 tx_pending_map; 	/* set for transmit buffer(s) pending a transmission */

	unsigned extra:1;	/* set when the delay queue tried reading CANINTF */
	unsigned skip;

	/* Message, transfer and buffers for one async spi transaction */
	struct spi_message message;
	struct spi_transfer transfer;
	u8 rx_buf[14] __attribute__((aligned(8)));
	u8 tx_buf[14] __attribute__((aligned(8)));

	struct tasklet_struct tasklet;
	struct timer_list timer;
	struct delayed_work delay;
};

static struct can_bittiming_const mcp2515_bittiming_const = {
	.name = KBUILD_MODNAME,
	.tseg1_min = 3,
	.tseg1_max = 16,
	.tseg2_min = 2,
	.tseg2_max = 8,
	.sjw_max = 4,
	.brp_min = 1,
	.brp_max = 64,
	.brp_inc = 1,
};

enum mcp2515_model {
	CAN_MCP2515	= 0x2515,
	CAN_MCP25625	= 0x25625,
};

/*
 * SPI asynchronous completion callback functions.
 */
static void mcp2515_read_flags_complete(void *context);
static void mcp2515_read_rxb0_complete(void *context);
static void mcp2515_read_rxb1_complete(void *context);
static void mcp2515_clear_canintf_complete(void *context);
static void mcp2515_clear_eflg_complete(void *context);
static void mcp2515_load_txb_complete(void *context);
static void mcp2515_rts_txb_complete(void *context);

/*
 * Write VALUE to register at address ADDR.
 * Synchronous.
 */
static int mcp2515_write_reg(struct spi_device *spi, u8 reg, u8 val)
{
	const u8 buf[] __attribute__((aligned(8))) = {
		[0] = MCP2515_INSTRUCTION_WRITE,
		[1] = reg,	/* address */
		[2] = val,	/* data */
	};

	return spi_write(spi, buf, sizeof(buf));
}

/*
 * Read VALUE from register at address ADDR.
 * Synchronous.
 */
static int mcp2515_read_reg(struct spi_device *spi, u8 reg, u8 *val)
{
	const u8 buf[] __attribute__((aligned(8))) = {
		[0] = MCP2515_INSTRUCTION_READ,
		[1] = reg,	/* address */
	};

	return spi_write_then_read(spi, buf, sizeof(buf), val, sizeof(*val));
}

static int mcp2515_read_2regs(struct spi_device *spi, u8 reg, u8 *v1, u8 *v2)
{
	const u8 tx_buf[] __attribute__((aligned(8))) = {
		[0] = MCP2515_INSTRUCTION_READ,
		[1] = reg,	/* address */
	};
	u8 rx_buf[2] __attribute__((aligned(8)));
	int err;

	err = spi_write_then_read(spi, tx_buf, sizeof(tx_buf),
				  rx_buf, sizeof(rx_buf));
	if (err)
		return err;

	*v1 = rx_buf[0];
	*v2 = rx_buf[1];

	return 0;
}

/*
 * Reset internal registers to default state and enter configuration mode.
 * Synchronous.
 */
static int mcp2515_hw_reset(struct spi_device *spi)
{
	const u8 cmd = MCP2515_INSTRUCTION_RESET;

	return spi_write(spi, &cmd, sizeof(cmd));
}

static int mcp2515_hw_sleep(struct spi_device *spi)
{
	return mcp2515_write_reg(spi, CANCTRL, CANCTRL_REQOP_SLEEP);
}

static int mcp2515_switch_regulator(struct regulator *reg, int on)
{
	if (IS_ERR_OR_NULL(reg))
		return 0;

	return on ? regulator_enable(reg) : regulator_disable(reg);
}

/*
 * Set the bit timing configuration registers, the interrupt enable register
 * and the receive buffers control registers.
 * Synchronous.
 */
static int mcp2515_chip_start(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	struct spi_device *spi = priv->spi;
	struct can_bittiming *bt = &priv->can.bittiming;
	unsigned long timeout;
	u8 *buf = (u8 *)priv->transfer.tx_buf;
	u8 mode;
	int err;

	err = mcp2515_hw_reset(spi);
	if (err)
		return err;

	/* set bittiming */
	buf[0] = MCP2515_INSTRUCTION_WRITE;
	buf[1] = CNF3;

	/* CNF3 */
	buf[2] = bt->phase_seg2 - 1;

	/* CNF2 */
	buf[3] = CNF2_BTLMODE |
		(priv->can.ctrlmode & CAN_CTRLMODE_3_SAMPLES ? CNF2_SAM : 0x0) |
		(bt->phase_seg1 - 1) << 3 | (bt->prop_seg - 1);

	/* CNF1 */
	buf[4] = (bt->sjw - 1) << 6 | (bt->brp - 1);

	/* CANINTE */
	buf[5] = CANINTE_RX | CANINTE_TX | CANINTE_ERR;

	netdev_info(dev, "writing CNF: 0x%02x 0x%02x 0x%02x\n",
		    buf[4], buf[3], buf[2]);
	err = spi_write(spi, buf, 6);
	if (err)
		return err;

	/* config RX buffers */
	/* buf[0] = MCP2515_INSTRUCTION_WRITE; already set */
	buf[1] = RXB0CTRL;

	/* RXB0CTRL */
	buf[2] = RXBCTRL_RXM1 | RXBCTRL_RXM0 | RXBCTRL_BUKT;

	/* RXB1CTRL */
	buf[3] = RXBCTRL_RXM1 | RXBCTRL_RXM0;

	err = spi_write(spi, buf, 4);
	if (err)
		return err;

	/* handle can.ctrlmode */
	if (priv->can.ctrlmode & CAN_CTRLMODE_LOOPBACK)
		mode = CANCTRL_REQOP_LOOPBACK;
	else if (priv->can.ctrlmode & CAN_CTRLMODE_LISTENONLY)
		mode = CANCTRL_REQOP_LISTEN_ONLY;
	else
		mode = CANCTRL_REQOP_NORMAL;

	if (mode & CAN_CTRLMODE_ONE_SHOT)
		mode |= CANCTRL_OSM;

	/* Put device into requested mode */
	mcp2515_switch_regulator(priv->transceiver, 1);
	mcp2515_write_reg(spi, CANCTRL, mode);

	/* Wait for the device to enter requested mode */
	timeout = jiffies + HZ;
	do {
		u8 reg_stat;

		err = mcp2515_read_reg(spi, CANSTAT, &reg_stat);
		if (err)
			goto failed_request;
		else if ((reg_stat & CANCTRL_REQOP_MASK) == mode)
			break;

		schedule();
		if (time_after(jiffies, timeout)) {
			dev_err(&spi->dev,
				"MCP2515 didn't enter in requested mode\n");
			err = -EBUSY;
			goto failed_request;
		}
	} while (1);

	priv->can.state = CAN_STATE_ERROR_ACTIVE;

	return 0;

 failed_request:
	mcp2515_switch_regulator(priv->transceiver, 0);

	return err;
}

static void mcp2515_chip_stop(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	struct spi_device *spi = priv->spi;

	mcp2515_hw_reset(spi);
	mcp2515_switch_regulator(priv->transceiver, 0);
	priv->can.state = CAN_STATE_STOPPED;

	return;
}

/*
 * Start an asynchronous SPI transaction.
 */
static void mcp2515_spi_async(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	int err;

	err = spi_async(priv->spi, &priv->message);
	if (err)
		netdev_err(dev, "%s failed with err=%d\n", __func__, err);
}

/*
 * Read CANINTF and EFLG registers in one shot.
 * Asynchronous.
 */
static void mcp2515_read_flags(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	u8 *buf = (u8 *)priv->transfer.tx_buf;
	int err;

	cancel_delayed_work(&priv->delay);
	buf[0] = MCP2515_INSTRUCTION_READ;
	buf[1] = CANINTF;
	buf[2] = 0;	/* CANINTF */
	buf[3] = 0;	/* EFLG */
	priv->transfer.len = 4;
	priv->message.complete = mcp2515_read_flags_complete;

	err = spi_async(priv->spi, &priv->message);
	if (err)
		netdev_err(dev, "%s failed with err=%d\n", __func__, err);
}

/*
 * Read receive buffer 0 (instruction 0x90) or 1 (instruction 0x94).
 * Asynchronous.
 */
static void mcp2515_read_rxb(struct net_device *dev, u8 instruction,
			     void (*complete)(void *))
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	u8 *buf = (u8 *)priv->transfer.tx_buf;

	memset(buf, 0, 14);
	buf[0] = instruction;
	priv->transfer.len = 14; /* instruction + id(4) + dlc + data(8) */
	priv->message.complete = complete;

	mcp2515_spi_async(dev);
}

/*
 * Read receive buffer 0.
 * Asynchronous.
 */
static void mcp2515_read_rxb0(struct net_device *dev)
{
	mcp2515_read_rxb(dev, MCP2515_INSTRUCTION_READ_RXB(0),
			 mcp2515_read_rxb0_complete);
}

/*
 * Read receive buffer 1.
 * Asynchronous.
 */
static void mcp2515_read_rxb1(struct net_device *dev)
{
	mcp2515_read_rxb(dev, MCP2515_INSTRUCTION_READ_RXB(1),
			 mcp2515_read_rxb1_complete);
}

/*
 * Clear CANINTF bits.
 * Asynchronous.
 */
static void mcp2515_clear_canintf(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	u8 *buf = (u8 *)priv->transfer.tx_buf;

	/* RX1IF & RX0IF flag cleared automatically during read */
	buf[0] = MCP2515_INSTRUCTION_BIT_MODIFY;
	buf[1] = CANINTF;
	buf[2] = priv->canintf & ~(CANINTF_RX0IF | CANINTF_RX1IF); /* mask */
	buf[3] = 0;	/* data */
	priv->transfer.len = 4;
	priv->message.complete = mcp2515_clear_canintf_complete;

	mcp2515_spi_async(dev);
}

/*
 * Clear EFLG bits.
 * Asynchronous.
 */
static void mcp2515_clear_eflg(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	u8 *buf = (u8 *)priv->transfer.tx_buf;

	buf[0] = MCP2515_INSTRUCTION_BIT_MODIFY;
	buf[1] = EFLAG;
	buf[2] = priv->eflg;	/* mask */
	buf[3] = 0;		/* data */
	priv->transfer.len = 4;
	priv->message.complete = mcp2515_clear_eflg_complete;

	mcp2515_spi_async(dev);
}

/*
 * Set the transmit buffer, starting at TXB0SIDH, for an skb.
 */
static int mcp2515_set_txbuf(u8 *buf, const struct sk_buff *skb)
{
	struct can_frame *frame = (struct can_frame *)skb->data;

	if (frame->can_id & CAN_EFF_FLAG) {
		buf[0] = frame->can_id >> 21;
		buf[1] = (frame->can_id >> 13 & 0xe0) | 8 |
			(frame->can_id >> 16 & 3);
		buf[2] = frame->can_id >> 8;
		buf[3] = frame->can_id;
	} else {
		buf[0] = frame->can_id >> 3;
		buf[1] = frame->can_id << 5;
		buf[2] = 0;
		buf[3] = 0;
	}

	buf[4] = frame->can_dlc;
	if (frame->can_id & CAN_RTR_FLAG)
		buf[4] |= 0x40;

	memcpy(buf + 5, frame->data, frame->can_dlc);

	return 5 + frame->can_dlc;
}

/*
 * Send the "load transmit buffer" SPI message.
 * Asynchronous.
 */
static void mcp2515_load_txb(struct sk_buff *skb, struct net_device *dev, u8 abc)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	u8 *buf = (u8 *)priv->transfer.tx_buf;

	buf[0] = MCP2515_INSTRUCTION_LOAD_TXB(abc);
	priv->transfer.len = mcp2515_set_txbuf(buf + 1, skb) + 1;
	priv->message.complete = mcp2515_load_txb_complete;
	priv->loaded_txb = abc;

	can_put_echo_skb(skb, dev, abc);

	mcp2515_spi_async(dev);
}

/*
 * Send the "request to send transmit buffer" SPI message.
 * Asynchronous.
 */
static void mcp2515_rts_txb(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	u8 *buf = (u8 *)priv->transfer.tx_buf;

	buf[0] = MCP2515_INSTRUCTION_RTS(priv->loaded_txb);
	priv->transfer.len = 1;
	priv->message.complete = mcp2515_rts_txb_complete;

	mcp2515_spi_async(dev);
}

/*
 * Called when the "read CANINTF and EFLG registers" SPI message completes.
 */
static void mcp2515_read_flags_complete(void *context)
{
	struct net_device *dev = context;
	struct mcp2515_priv *priv = netdev_priv(dev);
	u8 *buf = priv->transfer.rx_buf;
	unsigned canintf;

	priv->canintf = canintf = buf[2];
	priv->eflg = buf[3];

	/* We really ought never miss the edge triggered interrupt. But if we do, and the extra read is needed, note so here.*/
	if (priv->extra) {
		priv->extra = 0;
		if (priv->canintf != 0 || priv->eflg != 0)
			netdev_dbg(dev, "delayed read_flags detected a missed interrupt: CANINTF=%u, EFLG=%u\n", priv->canintf, priv->eflg);
	}

	/* We just read, delay reading again. */

	if (canintf & CANINTF_RX0IF)
		mcp2515_read_rxb0(dev);
	else if (canintf & CANINTF_RX1IF)
		mcp2515_read_rxb1(dev);
	else if (canintf)
		mcp2515_clear_canintf(dev);
	else {
		spin_lock_bh(&priv->lock);
		if (priv->tx_pending_map != 0) {
			u8 pending_tx_id = 0;
			for (; pending_tx_id < MCP2515_TX_CNT; pending_tx_id++) {
				if ((BIT(pending_tx_id) & priv->tx_pending_map)) {
					priv->tx_pending_map &= ~BIT(pending_tx_id);
					break;
				}
			}
			spin_unlock_bh(&priv->lock);
			mcp2515_load_txb(priv->skb[pending_tx_id], dev, pending_tx_id);
		} else if (priv->interrupt) {
			priv->interrupt = 0;
			spin_unlock_bh(&priv->lock);
			mcp2515_read_flags(dev);
		} else {
			priv->busy = 0;
			spin_unlock_bh(&priv->lock);
			/* Retry after READ_FLAGS_TIMER_VAL_MS. */
			mod_timer(&priv->timer, jiffies + READ_FLAGS_TIMER_VAL_MS);
		}
	}
}

/*
 * Called when one of the "read receive buffer i" SPI message completes.
 */
static void mcp2515_read_rxb_complete(void *context)
{
	struct net_device *dev = context;
	struct mcp2515_priv *priv = netdev_priv(dev);
	struct sk_buff *skb;
	struct can_frame *frame;
	u8 *buf = priv->transfer.rx_buf;

	skb = alloc_can_skb(dev, &frame);
	if (!skb) {
		dev->stats.rx_dropped++;
		return;
	}

	if (buf[2] & RXBSIDL_IDE) {
		frame->can_id = buf[1] << 21 | (buf[2] & 0xe0) << 13 |
			(buf[2] & 3) << 16 | buf[3] << 8 | buf[4] |
			CAN_EFF_FLAG;
		if (buf[5] & RXBDLC_RTR)
			frame->can_id |= CAN_RTR_FLAG;
	} else {
		frame->can_id = buf[1] << 3 | buf[2] >> 5;
		if (buf[2] & RXBSIDL_SRR)
			frame->can_id |= CAN_RTR_FLAG;
	}

	frame->can_dlc = get_can_dlc(buf[5] & 0xf);

	if (!(frame->can_id & CAN_RTR_FLAG))
		memcpy(frame->data, buf + 6, frame->can_dlc);

	dev->stats.rx_packets++;
	dev->stats.rx_bytes += frame->can_dlc;

	netif_rx_ni(skb);
}

/*
 * Transmit a frame if transmission pending, else read and process flags.
 */
static void mcp2515_transmit_or_read_flags(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);

	spin_lock_bh(&priv->lock);

	if (priv->tx_pending_map != 0) {
		u8 pending_tx_id = 0;
		for (; pending_tx_id < MCP2515_TX_CNT; pending_tx_id++) {
			if ((BIT(pending_tx_id) & priv->tx_pending_map)) {
				priv->tx_pending_map &= ~BIT(pending_tx_id);
				break;
			}
		}
		spin_unlock_bh(&priv->lock);
		mcp2515_load_txb(priv->skb[pending_tx_id], dev, pending_tx_id);
	} else {
		spin_unlock_bh(&priv->lock);
		mcp2515_read_flags(dev);
	}
}

/*
 * Called when the "read receive buffer 0" SPI message completes.
 */
static void mcp2515_read_rxb0_complete(void *context)
{
	struct net_device *dev = context;
	struct mcp2515_priv *priv = netdev_priv(dev);

	mcp2515_read_rxb_complete(context);

	if (priv->canintf & CANINTF_RX1IF)
		mcp2515_read_rxb1(dev);
	else
		mcp2515_transmit_or_read_flags(dev);
}

/*
 * Called when the "read receive buffer 1" SPI message completes.
 */
static void mcp2515_read_rxb1_complete(void *context)
{
	struct net_device *dev = context;

	mcp2515_read_rxb_complete(context);

	mcp2515_transmit_or_read_flags(dev);
}

static void mcp2515_update_device_stats(struct net_device *dev, struct sk_buff *skb, u8 idx)
{
	if (skb) {
		struct can_frame *f = (struct can_frame *)skb->data;
		dev->stats.tx_bytes += f->can_dlc;
		dev->stats.tx_packets++;
		can_get_echo_skb(dev, idx);
	}
}

/*
 * Called when the "clear CANINTF bits" SPI message completes.
 */
static void mcp2515_clear_canintf_complete(void *context)
{
	struct net_device *dev = context;
	struct mcp2515_priv *priv = netdev_priv(dev);

	if (priv->canintf & CANINTF_TX0IF) {
		mcp2515_update_device_stats(dev, priv->skb[0], 0);
		priv->skb[0] = NULL;
		priv->tx_busy_map &= ~BIT(0);
	}
	if (priv->canintf & CANINTF_TX1IF) {
		mcp2515_update_device_stats(dev, priv->skb[1], 1);
		priv->skb[1] = NULL;
		priv->tx_busy_map &= ~BIT(1);
	}
	if (priv->canintf & CANINTF_TX2IF) {
		mcp2515_update_device_stats(dev, priv->skb[2], 2);
		priv->skb[2] = NULL;
		priv->tx_busy_map &= ~BIT(2);
	}
	if (priv->netif_queue_stopped && priv->tx_busy_map < TX_MAP_BUSY) {
		priv->netif_queue_stopped = 0;
		netif_wake_queue(dev);
	}

	if (priv->eflg)
		mcp2515_clear_eflg(dev);
	else
		mcp2515_read_flags(dev);
}

/*
 * Called when the "clear EFLG bits" SPI message completes.
 */
static void mcp2515_clear_eflg_complete(void *context)
{
	struct net_device *dev = context;
	struct mcp2515_priv *priv = netdev_priv(dev);

	/*
	 * The receive flow chart (figure 4-3) of the data sheet (DS21801E)
	 * says that, if RXB0CTRL.BUKT is set (our case), the overflow
	 * flag that is set is EFLG.RX1OVR, when in fact it is EFLG.RX0OVR
	 * that is set.  To be safe, we test for any one of them.
	 */
	if (priv->eflg & (EFLG_RX0OVR | EFLG_RX1OVR))
		dev->stats.rx_over_errors++;

	mcp2515_read_flags(dev);
}

/*
 * Called when the "load transmit buffer" SPI message completes.
 */
static void mcp2515_load_txb_complete(void *context)
{
	struct net_device *dev = context;

	mcp2515_rts_txb(dev);
}

/*
 * Called when the "request to send transmit buffer" SPI message completes.
 */
static void mcp2515_rts_txb_complete(void *context)
{
	struct net_device *dev = context;

	mcp2515_read_flags(dev);
}

/*
 * Interrupt handler.
 */
static irqreturn_t mcp2515_interrupt(int irq, void *dev_id)
{
	struct net_device *dev = dev_id;
	struct mcp2515_priv *priv = netdev_priv(dev);

	tasklet_schedule(&priv->tasklet);

	return IRQ_HANDLED;
}

/*
 * IRQ handler, handles interrupts outside the hardware interrupt context.
 */
static void mcp2515_softirq_handler(unsigned long priv_arg)
{
	struct mcp2515_priv *priv = (struct mcp2515_priv *)priv_arg;

	spin_lock_bh(&priv->lock);
	if (priv->busy) {
		priv->interrupt = 1;
		spin_unlock_bh(&priv->lock);
		return;
	}
	priv->busy = 1;
	spin_unlock_bh(&priv->lock);

	mcp2515_read_flags(dev_get_drvdata(&priv->spi->dev));
}

/*
 * Timer callback, polls the MCP2515's interrupts.
 */
static void read_flags_timer_cb(struct timer_list *tmr)
{
	struct mcp2515_priv *priv;
	priv = container_of(tmr, struct mcp2515_priv, timer);

	spin_lock_bh(&priv->lock);
	if (priv->busy) {
		spin_unlock_bh(&priv->lock);
		if (++priv->skip > 10)
			netdev_dbg(dev_get_drvdata(&priv->spi->dev),
				"continually busy (now %i times)\n", priv->skip);
	} else {
		priv->busy = 1;
		spin_unlock_bh(&priv->lock);
		priv->skip = 0;
		priv->extra = 1;

		mcp2515_read_flags(dev_get_drvdata(&priv->spi->dev));
	}
}

/*
 * Transmit a frame.
 */
static netdev_tx_t mcp2515_start_xmit(struct sk_buff *skb,
				      struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	u8 tx_idx = 0;

	if (can_dropped_invalid_skb(dev, skb))
		return NETDEV_TX_OK;

	spin_lock_bh(&priv->lock);
	/* find a free tx slot */
	for (; tx_idx < MCP2515_TX_CNT; tx_idx++) {
		if ((BIT(tx_idx) & priv->tx_busy_map) == 0) {
			priv->tx_busy_map |= BIT(tx_idx);
			break;
		}
	}
	if (priv->tx_busy_map >= TX_MAP_BUSY) {
		priv->netif_queue_stopped = 1;
		netif_stop_queue(dev);
	}

	priv->skb[tx_idx] = skb;
	if (priv->busy) {
		priv->tx_pending_map |= BIT(tx_idx);
		spin_unlock_bh(&priv->lock);
		return NETDEV_TX_OK;
	}
	priv->busy = 1;
	spin_unlock_bh(&priv->lock);

	mcp2515_load_txb(skb, dev, tx_idx);

	return NETDEV_TX_OK;
}

/*
 * Called when the network device transitions to the up state.
 */
static int mcp2515_open(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	struct spi_device *spi = priv->spi;
	int err;

	mcp2515_switch_regulator(priv->power, 1);

	err = open_candev(dev);
	if (err)
		goto failed_open;

	err = request_irq(spi->irq, mcp2515_interrupt,
			  IRQF_TRIGGER_FALLING, dev->name, dev);
	if (err)
		goto failed_irq;

	err = mcp2515_chip_start(dev);
	if (err)
		goto failed_start;

	netif_start_queue(dev);

	return 0;

 failed_start:
	free_irq(spi->irq, dev);
 failed_irq:
	close_candev(dev);
 failed_open:
	mcp2515_hw_sleep(priv->spi);
	mcp2515_switch_regulator(priv->power, 0);
	return err;
}

/*
 * Called when the network device transitions to the down state.
 */
static int mcp2515_close(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	struct spi_device *spi = priv->spi;

	netif_stop_queue(dev);
	mcp2515_chip_stop(dev);
	free_irq(spi->irq, dev);

	mcp2515_hw_sleep(priv->spi);
	mcp2515_switch_regulator(priv->power, 0);

	close_candev(dev);

	return 0;
}

/*
 * Set up SPI messages.
 */
static void mcp2515_setup_spi_messages(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	struct device *device;
	void *buf;
	dma_addr_t dma;

	spi_message_init(&priv->message);
	priv->message.context = dev;

	/* FIXME */
	device = &priv->spi->dev;
	device->coherent_dma_mask = 0xffffffff;

	BUILD_BUG_ON(MCP2515_DMA_SIZE <
		     sizeof(priv->tx_buf) + sizeof(priv->rx_buf));

	buf = dma_alloc_coherent(device, MCP2515_DMA_SIZE, &dma, GFP_KERNEL);
	if (buf) {
		priv->transfer.tx_buf = buf;
		priv->transfer.rx_buf = buf + MCP2515_DMA_SIZE / 2;
		priv->transfer.tx_dma = dma;
		priv->transfer.rx_dma = dma + MCP2515_DMA_SIZE / 2;
		priv->message.is_dma_mapped = 1;
	} else {
		priv->transfer.tx_buf = priv->tx_buf;
		priv->transfer.rx_buf = priv->rx_buf;
	}

	spi_message_add_tail(&priv->transfer, &priv->message);
}

static void mcp2515_cleanup_spi_messages(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);

	if (!priv->message.is_dma_mapped)
		return;

	dma_free_coherent(&priv->spi->dev, MCP2515_DMA_SIZE,
			  (void *)priv->transfer.tx_buf, priv->transfer.tx_dma);
}

static int mcp2515_set_mode(struct net_device *dev, enum can_mode mode)
{
	int err;

	switch (mode) {
	case CAN_MODE_START:
		err = mcp2515_chip_start(dev);
		if (err)
			return err;

		netif_wake_queue(dev);
		break;

	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int mcp2515_get_berr_counter(const struct net_device *dev,
				    struct can_berr_counter *bec)
{
	const struct mcp2515_priv *priv = netdev_priv(dev);
	int err;
	u8 reg_tec, reg_rec;

	err = mcp2515_read_2regs(priv->spi, TEC, &reg_tec, &reg_rec);
	if (err)
		return err;

	bec->txerr = reg_tec;
	bec->rxerr = reg_rec;

	return 0;
}

/*
 * Network device operations.
 */
static const struct net_device_ops mcp2515_netdev_ops = {
	.ndo_open = mcp2515_open,
	.ndo_stop = mcp2515_close,
	.ndo_start_xmit = mcp2515_start_xmit,
};

static int mcp2515_register_candev(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	struct spi_device *spi = priv->spi;
	u8 reg_stat, reg_ctrl;
	int err = 0;

	mcp2515_switch_regulator(priv->power, 1);
	mcp2515_hw_reset(spi);

	/*
	 * Please note that these are "magic values" based on after
	 * reset defaults taken from data sheet which allows us to see
	 * if we really have a chip on the bus (we avoid common all
	 * zeroes or all ones situations)
	 */
	err |= mcp2515_read_reg(spi, CANSTAT, &reg_stat);
	err |= mcp2515_read_reg(spi, CANCTRL, &reg_ctrl);
	dev_dbg(&spi->dev, "%s: canstat=0x%02x canctrl=0x%02x\n",
		__func__, reg_stat, reg_ctrl);

	/* Check for power up default values */
	if (!((reg_stat & 0xee) == 0x80 && (reg_ctrl & 0x17) == 0x07) || err) {
		dev_err(&spi->dev, "%s: failed to detect chip"
			"(canstat=0x%02x, canctrl=0x%02x, err=%d)\n",
			__func__, reg_stat, reg_ctrl, err);
		err = -ENODEV;
		goto failed_detect;
	}

	err = register_candev(dev);
	if (err)
		goto failed_register;

	mcp2515_hw_sleep(priv->spi);
	mcp2515_switch_regulator(priv->power, 0);

	return 0;

 failed_register:
 failed_detect:
	mcp2515_hw_sleep(priv->spi);
	mcp2515_switch_regulator(priv->power, 0);

	return err;
}

static void mcp2515_unregister_candev(struct net_device *dev)
{
	unregister_candev(dev);
}

/*
 * Binds this driver to the spi device.
 */
static int mcp2515_probe(struct spi_device *spi)
{
	struct net_device *dev;
	struct mcp2515_priv *priv;
	struct clk *clk;
	u32 freq;
	int err;

	clk = devm_clk_get_optional(&spi->dev, NULL);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	freq = clk_get_rate(clk);
	if (freq == 0)
		device_property_read_u32(&spi->dev, "clock-frequency", &freq);

	/* Sanity check */
	if (freq < 1000000 || freq > 25000000)
		return -ERANGE;

	dev = alloc_candev(sizeof(struct mcp2515_priv), MCP2515_TX_CNT);
	if (!dev)
		return -ENOMEM;

	err = clk_prepare_enable(clk);
	if (err)
		goto out_free;

	dev_set_drvdata(&spi->dev, dev);
	SET_NETDEV_DEV(dev, &spi->dev);

	dev->netdev_ops = &mcp2515_netdev_ops;
	dev->flags |= IFF_ECHO;

	priv = netdev_priv(dev);
	priv->can.bittiming_const = &mcp2515_bittiming_const;
	priv->can.clock.freq = freq / 2;
	priv->can.ctrlmode_supported = CAN_CTRLMODE_LOOPBACK |
		CAN_CTRLMODE_LISTENONLY | CAN_CTRLMODE_3_SAMPLES |
		CAN_CTRLMODE_ONE_SHOT;
	priv->can.do_set_mode = mcp2515_set_mode;
	priv->can.do_get_berr_counter = mcp2515_get_berr_counter;
	priv->spi = spi;
	priv->clk = clk;

	spin_lock_init(&priv->lock);

	spi->bits_per_word = 8;
	spi->max_speed_hz = spi->max_speed_hz ? : 10000000;
	err = spi_setup(spi);
	if (err)
		goto out_clk;

	timer_setup(&priv->timer, read_flags_timer_cb, 0);

	tasklet_init(&priv->tasklet, mcp2515_softirq_handler, (unsigned long)priv);

	priv->power = devm_regulator_get_optional(&spi->dev, "vdd");
	priv->transceiver = devm_regulator_get_optional(&spi->dev, "xceiver");
	if ((PTR_ERR(priv->power) == -EPROBE_DEFER) ||
		(PTR_ERR(priv->transceiver) == -EPROBE_DEFER)) {
		err = -EPROBE_DEFER;
		goto out_irqs;
	}

	mcp2515_setup_spi_messages(dev);

	err = mcp2515_register_candev(dev);
	if (err) {
		netdev_err(dev, "registering netdev failed");
		err = -EPROBE_DEFER;
		goto out_spi;
	}

	netdev_info(dev, "device registered (cs=%u, irq=%d)\n",
			spi->chip_select, spi->irq);

	return 0;

 out_spi:
	mcp2515_cleanup_spi_messages(dev);
	dev_set_drvdata(&spi->dev, NULL);
 out_irqs:
 	del_timer(&priv->timer);
	tasklet_kill(&priv->tasklet);
 out_clk:
	clk_disable_unprepare(clk);
 out_free:
	free_candev(dev);
	dev_err(&spi->dev, "Probe failed, err=%d\n", -err);
	return err;
}

/*
 * Unbinds this driver from the spi device.
 */
static int mcp2515_remove(struct spi_device *spi)
{
	struct net_device *dev = dev_get_drvdata(&spi->dev);
	struct mcp2515_priv *priv = netdev_priv(dev);

	mcp2515_unregister_candev(dev);
	mcp2515_cleanup_spi_messages(dev);
	dev_set_drvdata(&spi->dev, NULL);
	free_candev(dev);

	del_timer(&priv->timer);
	tasklet_kill(&priv->tasklet);

	return 0;
}

static const struct of_device_id mcp2515_of_match[] = {
	{
		.compatible	= "microchip,mcp2515",
		.data		= (void *)CAN_MCP2515,
	},
	{
		.compatible	= "microchip,mcp25625",
		.data		= (void *)CAN_MCP25625,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, mcp2515_of_match);

static const struct spi_device_id mcp2515_id_table[] = {
	{
		.name		= "mcp2515",
		.driver_data	= (kernel_ulong_t)CAN_MCP2515,
	},
	{
		.name		= "mcp25625",
		.driver_data	= (kernel_ulong_t)CAN_MCP25625,
	},
	{ }
};
MODULE_DEVICE_TABLE(spi, mcp2515_id_table);

static struct spi_driver mcp2515_can_driver = {
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = mcp2515_of_match,
		.owner = THIS_MODULE,
	},
	.id_table = mcp2515_id_table,
	.probe = mcp2515_probe,
	.remove = mcp2515_remove,
};

module_spi_driver(mcp2515_can_driver);
