// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  PS3 gelic network driver.
 *
 * Copyright (C) 2007 Sony Computer Entertainment Inc.
 * Copyright 2006, 2007 Sony Corporation
 *
 * This file is based on: spider_net.c
 *
 * (C) Copyright IBM Corp. 2005
 *
 * Authors : Utz Bacher <utz.bacher@de.ibm.com>
 *           Jens Osterkamp <Jens.Osterkamp@de.ibm.com>
 */

#undef DEBUG

#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>

#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/if_vlan.h>

#include <linux/in.h>
#include <linux/ip.h>
#include <linux/tcp.h>

#include <linux/dma-mapping.h>
#include <net/checksum.h>
#include <asm/firmware.h>
#include <asm/ps3.h>
#include <asm/lv1call.h>

#include "ps3_gelic_net.h"
#include "ps3_gelic_wireless.h"

#define DRV_NAME "Gelic Network Driver"
#define DRV_VERSION "2.0"

MODULE_AUTHOR("SCE Inc.");
MODULE_DESCRIPTION("Gelic Network driver");
MODULE_LICENSE("GPL");

#define BUG_ON_DEBUG(_cond) do { \
	if (__is_defined(DEBUG)) \
		BUG_ON(_cond); \
	else \
		WARN_ON(_cond); \
} while (0)

int gelic_card_set_irq_mask(struct gelic_card *card, u64 mask)
{
	struct device *dev = ctodev(card);
	int status;

	status = lv1_net_set_interrupt_mask(bus_id(card), dev_id(card),
					    mask, 0);
	if (status) {
		dev_err(dev, "%s:%d failed: %d\n", __func__, __LINE__, status);
	}

	return status;
}

static void gelic_card_rx_irq_on(struct gelic_card *card)
{
	card->irq_mask |= GELIC_CARD_RXINT;
	gelic_card_set_irq_mask(card, card->irq_mask);
}

static void gelic_card_rx_irq_off(struct gelic_card *card)
{
	card->irq_mask &= ~GELIC_CARD_RXINT;
	gelic_card_set_irq_mask(card, card->irq_mask);
}

static void gelic_card_get_ether_port_status(struct gelic_card *card,
	int inform)
{
	u64 v2;
	struct net_device *ether_netdev;

	lv1_net_control(bus_id(card), dev_id(card),
		GELIC_LV1_GET_ETH_PORT_STATUS, GELIC_LV1_VLAN_TX_ETHERNET_0, 0,
		0, &card->ether_port_status, &v2);

	if (inform) {
		ether_netdev = card->netdev[GELIC_PORT_ETHERNET_0];
		if (card->ether_port_status & GELIC_LV1_ETHER_LINK_UP)
			netif_carrier_on(ether_netdev);
		else
			netif_carrier_off(ether_netdev);
	}
}

/**
 * gelic_descr_get_status -- returns the status of a descriptor
 * @descr: descriptor to look at
 *
 * returns the status as in the dmac_cmd_status field of the descriptor
 */
static enum gelic_descr_dma_status
gelic_descr_get_status(struct gelic_descr *descr)
{
	return be32_to_cpu(descr->hw_regs.dmac_cmd_status) &
		GELIC_DESCR_DMA_STAT_MASK;
}

static int gelic_card_set_link_mode(struct gelic_card *card, int mode)
{
	struct device *dev = ctodev(card);
	int status;
	u64 v1, v2;

	status = lv1_net_control(bus_id(card), dev_id(card),
		GELIC_LV1_SET_NEGOTIATION_MODE, GELIC_LV1_PHY_ETHERNET_0, mode,
		0, &v1, &v2);

	if (status) {
		dev_err(dev, "%s:%d: Failed setting negotiation mode: %d\n",
			__func__, __LINE__, status);
		return -EBUSY;
	}

	card->link_mode = mode;
	return 0;
}

/**
 * gelic_card_disable_txdmac - disables the transmit DMA controller
 * @card: card structure
 *
 * gelic_card_disable_txdmac terminates processing on the DMA controller by
 * turing off DMA and issuing a force end
 */
static void gelic_card_disable_txdmac(struct gelic_card *card)
{
	struct device *dev = ctodev(card);
	int status;

	/* this hvc blocks until the DMA in progress really stopped */
	status = lv1_net_stop_tx_dma(bus_id(card), dev_id(card));

	if (status) {
		dev_err(dev, "%s:%d: lv1_net_stop_tx_dma failed: %d\n",
			__func__, __LINE__, status);
	}
}

/**
 * gelic_card_enable_rxdmac - enables the receive DMA controller
 * @card: card structure
 *
 * gelic_card_enable_rxdmac enables the DMA controller by setting RX_DMA_EN
 * in the GDADMACCNTR register
 */
static void gelic_card_enable_rxdmac(struct gelic_card *card)
{
	struct device *dev = ctodev(card);
	int status;

	if (__is_defined(DEBUG) && (gelic_descr_get_status(card->rx_chain.head)
			!= GELIC_DESCR_DMA_CARDOWNED)) {
		dev_err(dev, "%s:%d: status=%x\n", __func__, __LINE__,
			be32_to_cpu(card->rx_chain.head->hw_regs.dmac_cmd_status));
		dev_err(dev, "%s:%d: nextphy=%x\n", __func__, __LINE__,
			be32_to_cpu(card->rx_chain.head->hw_regs.next_descr_addr));
		dev_err(dev, "%s:%d: head=%px\n", __func__, __LINE__,
			card->rx_chain.head);
	}

	status = lv1_net_start_rx_dma(bus_id(card), dev_id(card),
		card->rx_chain.head->link.cpu_addr, 0);

	if (status) {
		dev_err(dev, "%s:%d: lv1_net_start_rx_dma failed: %d\n",
			__func__, __LINE__, status);
	}
}

/**
 * gelic_card_disable_rxdmac - disables the receive DMA controller
 * @card: card structure
 *
 * gelic_card_disable_rxdmac terminates processing on the DMA controller by
 * turing off DMA and issuing a force end
 */
static void gelic_card_disable_rxdmac(struct gelic_card *card)
{
	struct device *dev = ctodev(card);
	int status;

	/* this hvc blocks until the DMA in progress really stopped */
	status = lv1_net_stop_rx_dma(bus_id(card), dev_id(card));

	if (status) {
		dev_err(dev, "%s:%d: lv1_net_stop_rx_dma failed: %d\n",
			__func__, __LINE__, status);
	}
}

/**
 * gelic_descr_set_status -- sets the status of a descriptor
 * @descr: descriptor to change
 * @status: status to set in the descriptor
 *
 * changes the status to the specified value. Doesn't change other bits
 * in the status
 */
static void gelic_descr_set_status(struct gelic_descr *descr,
	enum gelic_descr_dma_status status)
{
	descr->hw_regs.dmac_cmd_status = cpu_to_be32(status |
		(be32_to_cpu(descr->hw_regs.dmac_cmd_status) &
		~GELIC_DESCR_DMA_STAT_MASK));
	/*
	 * dma_cmd_status field is used to indicate whether the descriptor
	 * is valid or not.
	 * Usually caller of this function wants to inform that to the
	 * hardware, so we assure here the hardware sees the change.
	 */
	wmb();
}

/**
 * gelic_card_reset_chain - reset status of a descriptor chain
 * @card: card structure
 * @chain: address of chain
 * @start_descr: address of descriptor array
 *
 * Reset the status of dma descriptors to ready state
 * and re-initialize the hardware chain for later use
 */
static void gelic_card_reset_chain(struct gelic_card *card,
	struct gelic_descr_chain *chain, struct gelic_descr *start_descr)
{
	struct gelic_descr *descr;

	for (descr = start_descr; start_descr != descr->next; descr++) {
		gelic_descr_set_status(descr, GELIC_DESCR_DMA_CARDOWNED);
		descr->hw_regs.next_descr_addr =
			cpu_to_be32(descr->next->link.cpu_addr);
	}

	chain->head = start_descr;
	chain->tail = (descr - 1);

	(descr - 1)->hw_regs.next_descr_addr = 0;
}

void gelic_card_up(struct gelic_card *card)
{
	struct device *dev = ctodev(card);

	mutex_lock(&card->updown_lock);
	if (atomic_inc_return(&card->users) == 1) {
		dev_dbg(dev, "%s:%d: Starting...\n", __func__, __LINE__);
		gelic_card_set_irq_mask(card, card->irq_mask);
		gelic_card_enable_rxdmac(card);
		napi_enable(&card->napi);
	}
	mutex_unlock(&card->updown_lock);
}

void gelic_card_down(struct gelic_card *card)
{
	struct device *dev = ctodev(card);
	u64 mask;

	mutex_lock(&card->updown_lock);
	if (atomic_dec_if_positive(&card->users) == 0) {
		dev_dbg(dev, "%s:%d: Stopping...\n", __func__, __LINE__);
		napi_disable(&card->napi);
		/*
		 * Disable irq. Wireless interrupts will be disabled later.
		 */
		mask = card->irq_mask & (GELIC_CARD_WLAN_EVENT_RECEIVED |
			GELIC_CARD_WLAN_COMMAND_COMPLETED);
		gelic_card_set_irq_mask(card, mask);
		gelic_card_disable_rxdmac(card);
		gelic_card_reset_chain(card, &card->rx_chain,
			card->descr + GELIC_NET_TX_DESCRIPTORS);
		gelic_card_disable_txdmac(card);
	}
	mutex_unlock(&card->updown_lock);
}

static void gelic_unmap_link(struct device *dev, struct gelic_descr *descr)
{
	BUG_ON_DEBUG(descr->hw_regs.payload.dev_addr);
	BUG_ON_DEBUG(descr->hw_regs.payload.size);

	BUG_ON_DEBUG(!descr->link.cpu_addr);
	BUG_ON_DEBUG(!descr->link.size);

	dma_unmap_single(dev, descr->link.cpu_addr, descr->link.size,
		DMA_BIDIRECTIONAL);

	descr->link.cpu_addr = 0;
	descr->link.size = 0;
}

/**
 * gelic_card_free_chain - free descriptor chain
 * @card: card structure
 * @descr_in: address of desc
 */
static void gelic_card_free_chain(struct gelic_card *card,
	struct gelic_descr *descr_in)
{
	struct device *dev = ctodev(card);
	struct gelic_descr *descr;

	for (descr = descr_in; descr && descr->link.cpu_addr;
		descr = descr->next) {
		gelic_unmap_link(dev, descr);
	}
}

/**
 * gelic_card_init_chain - links descriptor chain
 * @card: card structure
 * @chain: address of chain
 * @start_descr: address of descriptor array
 * @descr_count: number of descriptors
 *
 * we manage a circular list that mirrors the hardware structure,
 * except that the hardware uses bus addresses.
 *
 * returns 0 on success, <0 on failure
 */
static int gelic_card_init_chain(struct gelic_card *card,
	struct gelic_descr_chain *chain, struct gelic_descr *start_descr,
	int descr_count)
{
	struct gelic_descr *descr = start_descr;
	struct device *dev = ctodev(card);
	unsigned int index;

	memset(start_descr, 0, descr_count * sizeof(*start_descr));

	for (index = 0, descr = start_descr; index < descr_count;
		index++, descr++) {
		gelic_descr_set_status(descr, GELIC_DESCR_DMA_NOT_IN_USE);

		descr->link.size = sizeof(struct gelic_hw_regs);
		descr->link.cpu_addr = dma_map_single(dev, descr,
			descr->link.size, DMA_BIDIRECTIONAL);

		if (unlikely(dma_mapping_error(dev, descr->link.cpu_addr))) {
			dev_err(dev, "%s:%d: dma_mapping_error\n", __func__,
				__LINE__);

			for (index--, descr--; index > 0; index--, descr--) {
				if (descr->link.cpu_addr) {
					gelic_unmap_link(dev, descr);
				}
			}
			return -ENOMEM;
		}

		descr->next = descr + 1;
		descr->prev = descr - 1;
	}

	(descr - 1)->next = start_descr;
	start_descr->prev = (descr - 1);

	/* chain bus addr of hw descriptor */
	for (index = 0, descr = start_descr; index < descr_count;
		index++, descr++) {
		descr->hw_regs.next_descr_addr =
			cpu_to_be32(descr->next->link.cpu_addr);
	}

	chain->head = start_descr;
	chain->tail = start_descr;

	/* do not chain last hw descriptor */
	(descr - 1)->hw_regs.next_descr_addr = 0;

	return 0;
}

/**
 * gelic_descr_prepare_rx - reinitializes a rx descriptor
 * @card: card structure
 * @descr: descriptor to re-init
 *
 * return 0 on success, <0 on failure
 *
 * allocates a new rx skb, iommu-maps it and attaches it to the descriptor.
 * Activate the descriptor state-wise
 */
static int gelic_descr_prepare_rx(struct gelic_card *card,
	struct gelic_descr *descr)
{
	struct device *dev = ctodev(card);
	struct aligned_buff {
		unsigned int total_bytes;
		unsigned int offset;
	};
	struct aligned_buff a_buf;
	dma_addr_t cpu_addr;

	if (gelic_descr_get_status(descr) !=  GELIC_DESCR_DMA_NOT_IN_USE) {
		dev_err(dev, "%s:%d: ERROR status\n", __func__, __LINE__);
	}

	a_buf.total_bytes = ALIGN(GELIC_NET_MAX_MTU, GELIC_NET_RXBUF_ALIGN)
		+ GELIC_NET_RXBUF_ALIGN;

	descr->skb = dev_alloc_skb(a_buf.total_bytes);

	if (!descr->skb) {
		descr->hw_regs.payload.dev_addr = 0;
		descr->hw_regs.payload.size = 0;
		return -ENOMEM;
	}

	a_buf.offset = PTR_ALIGN(descr->skb->data, GELIC_NET_RXBUF_ALIGN)
		- descr->skb->data;

	if (a_buf.offset) {
		dev_dbg(dev, "%s:%d: offset=%u\n", __func__, __LINE__,
			a_buf.offset);
		skb_reserve(descr->skb, a_buf.offset);
	}

	descr->hw_regs.dmac_cmd_status = 0;
	descr->hw_regs.result_size = 0;
	descr->hw_regs.valid_size = 0;
	descr->hw_regs.data_error = 0;

	descr->hw_regs.payload.size = a_buf.total_bytes - a_buf.offset;
	cpu_addr = dma_map_single(dev, descr->skb->data,
		descr->hw_regs.payload.size, DMA_FROM_DEVICE);
	descr->hw_regs.payload.dev_addr = cpu_to_be32(cpu_addr);

	if (unlikely(dma_mapping_error(dev, cpu_addr))) {
		dev_err(dev, "%s:%d: dma_mapping_error\n", __func__, __LINE__);

		descr->hw_regs.payload.dev_addr = 0;
		descr->hw_regs.payload.size = 0;

		dev_kfree_skb_any(descr->skb);
		descr->skb = NULL;

		gelic_descr_set_status(descr, GELIC_DESCR_DMA_NOT_IN_USE);

		return -ENOMEM;
	}

	gelic_descr_set_status(descr, GELIC_DESCR_DMA_CARDOWNED);
	return 0;
}

/**
 * gelic_card_release_rx_chain - free all skb of rx descr
 * @card: card structure
 *
 */
static void gelic_card_release_rx_chain(struct gelic_card *card)
{
	struct gelic_descr *descr = card->rx_chain.head;
	struct device *dev = ctodev(card);

	do {
		if (descr->skb) {
			dma_unmap_single(dev,
				be32_to_cpu(descr->hw_regs.payload.dev_addr),
				descr->hw_regs.payload.size, DMA_FROM_DEVICE);

			dev_kfree_skb_any(descr->skb);
			descr->skb = NULL;

			gelic_descr_set_status(descr,
				GELIC_DESCR_DMA_NOT_IN_USE);
		}

		descr->hw_regs.payload.dev_addr = 0;
		descr->hw_regs.payload.size = 0;

		descr = descr->next;
	} while (descr != card->rx_chain.head);
}

/**
 * gelic_card_fill_rx_chain - fills descriptors/skbs in the rx chains
 * @card: card structure
 *
 * fills all descriptors in the rx chain: allocates skbs
 * and iommu-maps them.
 * returns 0 on success, < 0 on failure
 */
static int gelic_card_fill_rx_chain(struct gelic_card *card)
{
	struct gelic_descr *descr = card->rx_chain.head;
	int ret;

	do {
		if (!descr->skb) {
			ret = gelic_descr_prepare_rx(card, descr);
			if (ret)
				goto rewind;
		}
		descr = descr->next;
	} while (descr != card->rx_chain.head);

	return 0;
rewind:
	gelic_card_release_rx_chain(card);
	return ret;
}

/**
 * gelic_card_alloc_rx_skbs - allocates rx skbs in rx descriptor chains
 * @card: card structure
 *
 * returns 0 on success, < 0 on failure
 */
static int gelic_card_alloc_rx_skbs(struct gelic_card *card)
{
	struct gelic_descr_chain *chain;
	int ret;
	chain = &card->rx_chain;
	ret = gelic_card_fill_rx_chain(card);
	chain->tail = card->rx_top->prev; /* point to the last */
	return ret;
}

/**
 * gelic_descr_release_tx - processes a used tx descriptor
 * @card: card structure
 * @descr: descriptor to release
 *
 * releases a used tx descriptor (unmapping, freeing of skb)
 */
static void gelic_descr_release_tx(struct gelic_card *card,
	struct gelic_descr *descr)
{
	struct sk_buff *skb = descr->skb;
	struct device *dev = ctodev(card);

	BUG_ON_DEBUG(!(be32_to_cpu(descr->hw_regs.data_status) &
		GELIC_DESCR_TX_TAIL));

	dma_unmap_single(dev, be32_to_cpu(descr->hw_regs.payload.dev_addr),
		descr->hw_regs.payload.size, DMA_TO_DEVICE);

	descr->hw_regs.payload.dev_addr = 0;
	descr->hw_regs.payload.size = 0;

	dev_kfree_skb_any(skb);
	descr->skb = NULL;

	descr->hw_regs.next_descr_addr = 0;
	descr->hw_regs.result_size = 0;
	descr->hw_regs.valid_size = 0;
	descr->hw_regs.data_status = 0;
	descr->hw_regs.data_error = 0;

	gelic_descr_set_status(descr, GELIC_DESCR_DMA_NOT_IN_USE);
}

static void gelic_card_stop_queues(struct gelic_card *card)
{
	netif_stop_queue(card->netdev[GELIC_PORT_ETHERNET_0]);

	if (card->netdev[GELIC_PORT_WIRELESS])
		netif_stop_queue(card->netdev[GELIC_PORT_WIRELESS]);
}
static void gelic_card_wake_queues(struct gelic_card *card)
{
	netif_wake_queue(card->netdev[GELIC_PORT_ETHERNET_0]);

	if (card->netdev[GELIC_PORT_WIRELESS])
		netif_wake_queue(card->netdev[GELIC_PORT_WIRELESS]);
}
/**
 * gelic_card_release_tx_chain - processes sent tx descriptors
 * @card: adapter structure
 * @stop: net_stop sequence
 *
 * releases the tx descriptors that gelic has finished with
 */
static void gelic_card_release_tx_chain(struct gelic_card *card, int stop)
{
	struct gelic_descr_chain *tx_chain;
	struct device *dev = ctodev(card);
	int release;

	for (release = 0, tx_chain = &card->tx_chain;
		tx_chain->head != tx_chain->tail && tx_chain->tail;
		tx_chain->tail = tx_chain->tail->next) {
		enum gelic_descr_dma_status status;
		struct gelic_descr *descr;
		struct net_device *netdev;

		descr = tx_chain->tail;
		status = gelic_descr_get_status(descr);
		netdev = descr->skb->dev;

		switch (status) {
		case GELIC_DESCR_DMA_RESPONSE_ERROR:
		case GELIC_DESCR_DMA_PROTECTION_ERROR:
		case GELIC_DESCR_DMA_FORCE_END:
			dev_info_ratelimited(dev,
				"%s:%d: forcing end of tx descriptor with status %x\n",
				__func__, __LINE__, status);
			netdev->stats.tx_dropped++;
			break;

		case GELIC_DESCR_DMA_COMPLETE:
			if (descr->skb) {
				netdev->stats.tx_packets++;
				netdev->stats.tx_bytes += descr->skb->len;
			}
			break;

		default:
			if (!stop) {
				goto out;
			}
		}

		gelic_descr_release_tx(card, descr);
		release++;
	}
out:
	if (!stop && release) {
		gelic_card_wake_queues(card);
	}
}

/**
 * gelic_net_set_multi - sets multicast addresses and promisc flags
 * @netdev: interface device structure
 *
 * gelic_net_set_multi configures multicast addresses as needed for the
 * netdev interface. It also sets up multicast, allmulti and promisc
 * flags appropriately
 */
void gelic_net_set_multi(struct net_device *netdev)
{
	struct gelic_card *card = netdev_card(netdev);
	struct device *dev = ctodev(card);
	struct netdev_hw_addr *ha;
	unsigned int i;
	uint8_t *p;
	u64 addr;
	int status;

	status = lv1_net_remove_multicast_address(bus_id(card), dev_id(card),
		0, 1);

	if (status) {
		dev_err(dev,
			"%s:%d: lv1_net_remove_multicast_address failed %d\n",
			__func__, __LINE__, status);
	}

	status = lv1_net_add_multicast_address(bus_id(card), dev_id(card),
		GELIC_NET_BROADCAST_ADDR, 0);

	if (status) {
		dev_err(dev,
			"%s:%d: lv1_net_add_multicast_address failed, %d\n",
			__func__, __LINE__, status);
	}

	if ((netdev->flags & IFF_ALLMULTI) ||
	    (netdev_mc_count(netdev) > GELIC_NET_MC_COUNT_MAX)) {
		status = lv1_net_add_multicast_address(bus_id(card),
			dev_id(card), 0, 1);

		if (status) {
			dev_err(dev,
				"%s:%d: lv1_net_add_multicast_address failed, %d\n",
				__func__, __LINE__, status);
		}
		return;
	}

	netdev_for_each_mc_addr(ha, netdev) {
		addr = 0;
		p = ha->addr;
		for (i = 0; i < ETH_ALEN; i++) {
			addr <<= 8;
			addr |= *p++;
		}
		status = lv1_net_add_multicast_address(bus_id(card),
			dev_id(card), addr, 0);

		if (status) {
			dev_err(dev,
				"%s:%d: lv1_net_add_multicast_address failed, %d\n",
				__func__, __LINE__, status);
		}
	}
}

/**
 * gelic_net_stop - called upon ifconfig down
 * @netdev: interface device structure
 *
 * always returns 0
 */
int gelic_net_stop(struct net_device *netdev)
{
	struct gelic_card *card = netdev_card(netdev);
	struct device *dev = ctodev(card);

	dev_dbg(dev, "%s:%d: >\n", __func__, __LINE__);

	netif_stop_queue(netdev);
	netif_carrier_off(netdev);

	gelic_card_down(card);

	dev_dbg(dev, "%s:%d: <\n", __func__, __LINE__);
	return 0;
}

/**
 * gelic_card_get_next_tx_descr - returns the next available tx descriptor
 * @card: device structure to get descriptor from
 *
 * returns the address of the next descriptor, or NULL if not available.
 */
static struct gelic_descr *gelic_card_get_next_tx_descr(struct gelic_card *card)
{
	if (!card->tx_chain.head)
		return NULL;

	/*  see if the next descriptor is free */
	if (card->tx_chain.tail != card->tx_chain.head->next &&
		(gelic_descr_get_status(card->tx_chain.head) ==
			GELIC_DESCR_DMA_NOT_IN_USE)) {
		return card->tx_chain.head;
	}

	return NULL;
}

/**
 * gelic_net_set_txdescr_cmdstat - sets the tx descriptor command field
 * @descr: descriptor structure to fill out
 * @skb: packet to consider
 *
 * fills out the command and status field of the descriptor structure,
 * depending on hardware checksum settings. This function assumes a wmb()
 * has executed before.
 */
static void gelic_descr_set_tx_cmdstat(struct gelic_descr *descr,
	struct sk_buff *skb)
{
	if (skb->ip_summed != CHECKSUM_PARTIAL)
		descr->hw_regs.dmac_cmd_status =
			cpu_to_be32(GELIC_DESCR_DMA_CMD_NO_CHKSUM |
				GELIC_DESCR_TX_DMA_FRAME_TAIL);
	else {
		/* is packet ip?
		 * if yes: tcp? udp? */
		if (skb->protocol == htons(ETH_P_IP)) {
			if (ip_hdr(skb)->protocol == IPPROTO_TCP)
				descr->hw_regs.dmac_cmd_status =
				cpu_to_be32(GELIC_DESCR_DMA_CMD_TCP_CHKSUM |
					    GELIC_DESCR_TX_DMA_FRAME_TAIL);

			else if (ip_hdr(skb)->protocol == IPPROTO_UDP)
				descr->hw_regs.dmac_cmd_status =
				cpu_to_be32(GELIC_DESCR_DMA_CMD_UDP_CHKSUM |
					    GELIC_DESCR_TX_DMA_FRAME_TAIL);
			else	/*
				 * the stack should checksum non-tcp and non-udp
				 * packets on his own: NETIF_F_IP_CSUM
				 */
				descr->hw_regs.dmac_cmd_status =
				cpu_to_be32(GELIC_DESCR_DMA_CMD_NO_CHKSUM |
					    GELIC_DESCR_TX_DMA_FRAME_TAIL);
		}
	}
}

static struct sk_buff *gelic_put_vlan_tag(struct sk_buff *skb,
	unsigned short tag)
{
	struct vlan_ethhdr *veth;
	static unsigned int c;

	if (skb_headroom(skb) < VLAN_HLEN) {
		struct sk_buff *sk_tmp = skb;
		pr_debug("%s:%d: hd=%d c=%ud\n", __func__, __LINE__,
			skb_headroom(skb), c);
		skb = skb_realloc_headroom(sk_tmp, VLAN_HLEN);
		if (!skb)
			return NULL;
		dev_kfree_skb_any(sk_tmp);
	}
	veth = skb_push(skb, VLAN_HLEN);

	/* Move the mac addresses to the top of buffer */
	memmove(skb->data, skb->data + VLAN_HLEN, 2 * ETH_ALEN);

	veth->h_vlan_proto = cpu_to_be16(ETH_P_8021Q);
	veth->h_vlan_TCI = htons(tag);

	return skb;
}

/**
 * gelic_descr_prepare_tx - setup a descriptor for sending packets
 * @card: card structure
 * @descr: descriptor structure
 * @skb: packet to use
 *
 * returns 0 on success, <0 on failure.
 *
 */
static int gelic_descr_prepare_tx(struct gelic_card *card,
	struct gelic_descr *descr, struct sk_buff *skb)
{
	struct device *dev = ctodev(card);
	dma_addr_t cpu_addr;

	if (card->vlan_required) {
		struct sk_buff *skb_tmp;
		enum gelic_port_type type;

		type = netdev_port(skb->dev)->type;
		skb_tmp = gelic_put_vlan_tag(skb, card->vlan[type].tx);

		if (!skb_tmp) {
			return -ENOMEM;
		}

		skb = skb_tmp;
	}

	descr->hw_regs.payload.size = skb->len;
	cpu_addr = dma_map_single(dev, skb->data, descr->hw_regs.payload.size,
		DMA_TO_DEVICE);
	descr->hw_regs.payload.dev_addr = cpu_to_be32(cpu_addr);

	if (unlikely(dma_mapping_error(dev, cpu_addr))) {
		dev_err(dev, "%s:%d: dma_mapping_error\n", __func__, __LINE__);

		descr->hw_regs.payload.dev_addr = 0;
		descr->hw_regs.payload.size = 0;
		return -ENOMEM;
	}

	descr->skb = skb;
	descr->hw_regs.data_status = 0;
	descr->hw_regs.next_descr_addr = 0; /* terminate hw descr */
	gelic_descr_set_tx_cmdstat(descr, skb);

	/* bump free descriptor pointer */
	card->tx_chain.head = descr->next;
	return 0;
}

/**
 * gelic_card_kick_txdma - enables TX DMA processing
 * @card: card structure
 * @descr: descriptor address to enable TX processing at
 *
 */
static int gelic_card_kick_txdma(struct gelic_card *card,
				 struct gelic_descr *descr)
{
	struct device *dev = ctodev(card);
	int status = 0;

	if (card->tx_dma_progress)
		return 0;

	if (gelic_descr_get_status(descr) == GELIC_DESCR_DMA_CARDOWNED) {
		card->tx_dma_progress = 1;
		status = lv1_net_start_tx_dma(bus_id(card), dev_id(card),
					      descr->link.cpu_addr, 0);
		if (status) {
			card->tx_dma_progress = 0;
			dev_info(dev, "%s:%d: lv1_net_start_txdma failed: %d\n",
				__func__, __LINE__, status);
		}
	}
	return status;
}

/**
 * gelic_net_xmit - transmits a frame over the device
 * @skb: packet to send out
 * @netdev: interface device structure
 *
 * returns NETDEV_TX_OK on success, NETDEV_TX_BUSY on failure
 */
netdev_tx_t gelic_net_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct gelic_card *card = netdev_card(netdev);
	struct device *dev = ctodev(card);
	struct gelic_descr *descr;
	int result;
	unsigned long flags;

	spin_lock_irqsave(&card->tx_lock, flags);

	gelic_card_release_tx_chain(card, 0);

	descr = gelic_card_get_next_tx_descr(card);
	if (!descr) {
		/*
		 * no more descriptors free
		 */
		gelic_card_stop_queues(card);
		spin_unlock_irqrestore(&card->tx_lock, flags);
		return NETDEV_TX_BUSY;
	}

	result = gelic_descr_prepare_tx(card, descr, skb);
	if (result) {
		/*
		 * DMA map failed.  As chances are that failure
		 * would continue, just release skb and return
		 */
		netdev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		spin_unlock_irqrestore(&card->tx_lock, flags);
		return NETDEV_TX_OK;
	}
	/*
	 * link this prepared descriptor to previous one
	 * to achieve high performance
	 */
	descr->prev->hw_regs.next_descr_addr =
		cpu_to_be32(descr->link.cpu_addr);
	/*
	 * as hardware descriptor is modified in the above lines,
	 * ensure that the hardware sees it
	 */
	wmb();
	if (gelic_card_kick_txdma(card, descr)) {
		/*
		 * kick failed.
		 * release descriptor which was just prepared
		 */
		netdev->stats.tx_dropped++;
		/* don't trigger BUG_ON() in gelic_descr_release_tx */
		descr->hw_regs.data_status = cpu_to_be32(GELIC_DESCR_TX_TAIL);
		gelic_descr_release_tx(card, descr);
		/* reset head */
		card->tx_chain.head = descr;
		/* reset hw termination */
		descr->prev->hw_regs.next_descr_addr = 0;
		dev_info(dev, "%s:%d: kick failure\n", __func__, __LINE__);
	}

	spin_unlock_irqrestore(&card->tx_lock, flags);
	return NETDEV_TX_OK;
}

/**
 * gelic_net_pass_skb_up - takes an skb from a descriptor and passes it on
 * @descr: descriptor to process
 * @card: card structure
 * @netdev: net_device structure to be passed packet
 *
 * iommu-unmaps the skb, fills out skb structure and passes the data to the
 * stack. The descriptor state is not changed.
 */
static void gelic_net_pass_skb_up(struct gelic_descr *descr,
	struct gelic_card *card, struct net_device *netdev)
{
	struct device *dev = ctodev(card);
	struct sk_buff *skb = descr->skb;
	u32 data_status, data_error;

	data_status = be32_to_cpu(descr->hw_regs.data_status);
	data_error = be32_to_cpu(descr->hw_regs.data_error);

	dma_unmap_single(dev, be32_to_cpu(descr->hw_regs.payload.dev_addr),
			 descr->hw_regs.payload.size, DMA_FROM_DEVICE);

	skb_put(skb, be32_to_cpu(descr->hw_regs.valid_size) ?
		be32_to_cpu(descr->hw_regs.valid_size) :
		be32_to_cpu(descr->hw_regs.result_size));

	if (!descr->hw_regs.valid_size) {
		dev_err(dev, "%s:%d: buffer full %x %x %x\n", __func__,
			__LINE__,
			be32_to_cpu(descr->hw_regs.result_size),
			be32_to_cpu(descr->hw_regs.payload.size),
			be32_to_cpu(descr->hw_regs.dmac_cmd_status));
	}

	descr->skb = NULL;
	/*
	 * the card put 2 bytes vlan tag in front
	 * of the ethernet frame
	 */
	skb_pull(skb, 2);
	skb->protocol = eth_type_trans(skb, netdev);

	/* checksum offload */
	if (netdev->features & NETIF_F_RXCSUM) {
		if ((data_status & GELIC_DESCR_DATA_STATUS_CHK_MASK) &&
		    (!(data_error & GELIC_DESCR_DATA_ERROR_CHK_MASK)))
			skb->ip_summed = CHECKSUM_UNNECESSARY;
		else
			skb_checksum_none_assert(skb);
	} else
		skb_checksum_none_assert(skb);

	/* update netdevice statistics */
	netdev->stats.rx_packets++;
	netdev->stats.rx_bytes += skb->len;

	/* pass skb up to stack */
	netif_receive_skb(skb);
}

/**
 * gelic_card_decode_one_descr - processes an rx descriptor
 * @card: card structure
 *
 * returns 1 if a packet has been sent to the stack, otherwise 0
 *
 * processes an rx descriptor by iommu-unmapping the data buffer and passing
 * the packet up to the stack
 */
static int gelic_card_decode_one_descr(struct gelic_card *card)
{
	enum gelic_descr_dma_status status;
	struct gelic_descr_chain *chain = &card->rx_chain;
	struct gelic_descr *descr = chain->head;
	struct device *dev = ctodev(card);
	struct net_device *netdev = NULL;
	int dmac_chain_ended;

	status = gelic_descr_get_status(descr);

	if (status == GELIC_DESCR_DMA_CARDOWNED)
		return 0;

	if (status == GELIC_DESCR_DMA_NOT_IN_USE) {
		dev_dbg(dev, "%s:%d: dormant descr? %px\n", __func__, __LINE__,
			descr);
		return 0;
	}

	/* netdevice select */
	if (card->vlan_required) {
		unsigned int i;
		u16 vid;
		vid = *(u16 *)(descr->skb->data) & VLAN_VID_MASK;
		for (i = 0; i < GELIC_PORT_MAX; i++) {
			if (card->vlan[i].rx == vid) {
				netdev = card->netdev[i];
				break;
			}
		}
		if (GELIC_PORT_MAX <= i) {
			dev_info(dev, "%s:%d: unknown packet vid=%x\n",
				__func__, __LINE__, vid);
			goto refill;
		}
	} else
		netdev = card->netdev[GELIC_PORT_ETHERNET_0];

	if ((status == GELIC_DESCR_DMA_RESPONSE_ERROR) ||
		(status == GELIC_DESCR_DMA_PROTECTION_ERROR) ||
		(status == GELIC_DESCR_DMA_FORCE_END)) {
		dev_info(dev, "%s:%d: dropping RX descriptor with state %x\n",
			__func__, __LINE__, status);
		netdev->stats.rx_dropped++;
		goto refill;
	}

	if (status == GELIC_DESCR_DMA_BUFFER_FULL) {
		/*
		 * Buffer full would occur if and only if
		 * the frame length was longer than the size of this
		 * descriptor's buffer.  If the frame length was equal
		 * to or shorter than buffer'size, FRAME_END condition
		 * would occur.
		 * Anyway this frame was longer than the MTU,
		 * just drop it.
		 */
		dev_info(dev, "%s:%d: overlength frame\n", __func__, __LINE__);
		goto refill;
	}
	/*
	 * descriptors any other than FRAME_END here should
	 * be treated as error.
	 */
	if (status != GELIC_DESCR_DMA_FRAME_END) {
		dev_dbg(dev, "%s:%d: RX descriptor with state %x\n", __func__,
			__LINE__, status);
		goto refill;
	}

	/* ok, we've got a packet in descr */
	gelic_net_pass_skb_up(descr, card, netdev);
refill:

	/* is the current descriptor terminated with next_descr == NULL? */
	dmac_chain_ended = be32_to_cpu(descr->hw_regs.dmac_cmd_status) &
		GELIC_DESCR_RX_DMA_CHAIN_END;
	/*
	 * So that always DMAC can see the end
	 * of the descriptor chain to avoid
	 * from unwanted DMAC overrun.
	 */
	descr->hw_regs.next_descr_addr = 0;

	/* change the descriptor state: */
	gelic_descr_set_status(descr, GELIC_DESCR_DMA_NOT_IN_USE);

	/*
	 * this call can fail, but for now, just leave this
	 * descriptor without skb
	 */
	gelic_descr_prepare_rx(card, descr);

	chain->tail = descr;
	chain->head = descr->next;

	/*
	 * Set this descriptor the end of the chain.
	 */
	descr->prev->hw_regs.next_descr_addr =
		cpu_to_be32(descr->link.cpu_addr);

	/*
	 * If dmac chain was met, DMAC stopped.
	 * thus re-enable it
	 */

	if (dmac_chain_ended) {
		gelic_card_enable_rxdmac(card);
	}

	return 1;
}

/**
 * gelic_net_poll - NAPI poll function called by the stack to return packets
 * @napi: napi structure
 * @budget: number of packets we can pass to the stack at most
 *
 * returns the number of the processed packets
 *
 */
static int gelic_net_poll(struct napi_struct *napi, int budget)
{
	struct gelic_card *card = container_of(napi, struct gelic_card, napi);
	int packets_done = 0;

	while (packets_done < budget) {
		if (!gelic_card_decode_one_descr(card)) {
			break;
		}
		packets_done++;
	}

	if (packets_done < budget) {
		napi_complete_done(napi, packets_done);
		gelic_card_rx_irq_on(card);
	}

	return packets_done;
}

/*
 * gelic_card_interrupt - event handler for gelic_net
 */
static irqreturn_t gelic_card_interrupt(int irq, void *ptr)
{
	unsigned long flags;
	struct gelic_card *card = ptr;
	u64 status;

	status = card->irq_status;

	if (!status) {
		return IRQ_NONE;
	}

	status &= card->irq_mask;

	if (status & GELIC_CARD_RXINT) {
		gelic_card_rx_irq_off(card);
		napi_schedule(&card->napi);
	}

	if (status & GELIC_CARD_TXINT) {
		spin_lock_irqsave(&card->tx_lock, flags);
		card->tx_dma_progress = 0;
		gelic_card_release_tx_chain(card, 0);
		/* kick outstanding tx descriptor if any */
		gelic_card_kick_txdma(card, card->tx_chain.tail);
		spin_unlock_irqrestore(&card->tx_lock, flags);
	}

	/* ether port status changed */
	if (status & GELIC_CARD_PORT_STATUS_CHANGED) {
		gelic_card_get_ether_port_status(card, 1);
	}

#ifdef CONFIG_GELIC_WIRELESS
	if (status & (GELIC_CARD_WLAN_EVENT_RECEIVED |
		GELIC_CARD_WLAN_COMMAND_COMPLETED)) {
		gelic_wl_interrupt(card->netdev[GELIC_PORT_WIRELESS], status);
	}
#endif

	return IRQ_HANDLED;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
/**
 * gelic_net_poll_controller - artificial interrupt for netconsole etc.
 * @netdev: interface device structure
 *
 * see Documentation/networking/netconsole.rst
 */
void gelic_net_poll_controller(struct net_device *netdev)
{
	struct gelic_card *card = netdev_card(netdev);

	gelic_card_set_irq_mask(card, 0);
	gelic_card_interrupt(netdev->irq, netdev);
	gelic_card_set_irq_mask(card, card->irq_mask);
}
#endif /* CONFIG_NET_POLL_CONTROLLER */

/**
 * gelic_net_open - called upon ifconfig up
 * @netdev: interface device structure
 *
 * returns 0 on success, <0 on failure
 *
 * gelic_net_open allocates all the descriptors and memory needed for
 * operation, sets up multicast list and enables interrupts
 */
int gelic_net_open(struct net_device *netdev)
{
	struct gelic_card *card = netdev_card(netdev);

	gelic_card_up(card);

	netif_start_queue(netdev);
	gelic_card_get_ether_port_status(card, 1);

	return 0;
}

void gelic_net_get_drvinfo(struct net_device *netdev,
	struct ethtool_drvinfo *info)
{
	strlcpy(info->driver, DRV_NAME, sizeof(info->driver));
	strlcpy(info->version, DRV_VERSION, sizeof(info->version));
}

static int gelic_ether_get_link_ksettings(struct net_device *netdev,
	struct ethtool_link_ksettings *cmd)
{
	struct gelic_card *card = netdev_card(netdev);
	struct device *dev = ctodev(card);
	u32 supported, advertising;

	gelic_card_get_ether_port_status(card, 0);

	if (card->ether_port_status & GELIC_LV1_ETHER_FULL_DUPLEX)
		cmd->base.duplex = DUPLEX_FULL;
	else
		cmd->base.duplex = DUPLEX_HALF;

	switch (card->ether_port_status & GELIC_LV1_ETHER_SPEED_MASK) {
	case GELIC_LV1_ETHER_SPEED_10:
		cmd->base.speed = SPEED_10;
		break;
	case GELIC_LV1_ETHER_SPEED_100:
		cmd->base.speed = SPEED_100;
		break;
	case GELIC_LV1_ETHER_SPEED_1000:
		cmd->base.speed = SPEED_1000;
		break;
	default:
		dev_dbg(dev, "%s:%d: speed unknown\n", __func__, __LINE__);
		cmd->base.speed = SPEED_10;
		break;
	}

	supported = SUPPORTED_TP | SUPPORTED_Autoneg |
		SUPPORTED_10baseT_Half | SUPPORTED_10baseT_Full |
		SUPPORTED_100baseT_Half | SUPPORTED_100baseT_Full |
		SUPPORTED_1000baseT_Full;

	advertising = supported;

	if (card->link_mode & GELIC_LV1_ETHER_AUTO_NEG) {
		cmd->base.autoneg = AUTONEG_ENABLE;
	} else {
		cmd->base.autoneg = AUTONEG_DISABLE;
		advertising &= ~ADVERTISED_Autoneg;
	}
	cmd->base.port = PORT_TP;

	ethtool_convert_legacy_u32_to_link_mode(cmd->link_modes.supported,
		supported);
	ethtool_convert_legacy_u32_to_link_mode(cmd->link_modes.advertising,
		advertising);

	return 0;
}

static int gelic_ether_set_link_ksettings(struct net_device *netdev,
	const struct ethtool_link_ksettings *cmd)
{
	struct gelic_card *card = netdev_card(netdev);
	struct device *dev = ctodev(card);
	u64 mode;
	int ret;

	if (cmd->base.autoneg == AUTONEG_ENABLE) {
		mode = GELIC_LV1_ETHER_AUTO_NEG;
	} else {
		switch (cmd->base.speed) {
		case SPEED_10:
			mode = GELIC_LV1_ETHER_SPEED_10;
			break;
		case SPEED_100:
			mode = GELIC_LV1_ETHER_SPEED_100;
			break;
		case SPEED_1000:
			mode = GELIC_LV1_ETHER_SPEED_1000;
			break;
		default:
			return -EINVAL;
		}

		if (cmd->base.duplex == DUPLEX_FULL) {
			mode |= GELIC_LV1_ETHER_FULL_DUPLEX;
		} else if (cmd->base.speed == SPEED_1000) {
			dev_dbg(dev,
				"%s:%d: 1000 half duplex is not supported.\n",
				__func__, __LINE__);
			return -EINVAL;
		}
	}

	ret = gelic_card_set_link_mode(card, mode);

	if (ret) {
		return ret;
	}

	return 0;
}

static void gelic_net_get_wol(struct net_device *netdev,
	struct ethtool_wolinfo *wol)
{
	if (ps3_compare_firmware_version(2, 2, 0) >= 0) {
		wol->supported = WAKE_MAGIC;
	} else {
		wol->supported = 0;
	}

	wol->wolopts = ps3_sys_manager_get_wol() ? wol->supported : 0;
	memset(&wol->sopass, 0, sizeof(wol->sopass));
}

static int gelic_net_set_wol(struct net_device *netdev,
	struct ethtool_wolinfo *wol)
{
	struct gelic_card *card = netdev_card(netdev);
	struct device *dev = ctodev(card);
	int status;
	u64 v1, v2;

	if (ps3_compare_firmware_version(2, 2, 0) < 0 ||
		!capable(CAP_NET_ADMIN)) {
		return -EPERM;
	}

	if (wol->wolopts & ~WAKE_MAGIC) {
		return -EINVAL;
	}

	if (wol->wolopts & WAKE_MAGIC) {
		status = lv1_net_control(bus_id(card), dev_id(card),
			GELIC_LV1_SET_WOL, GELIC_LV1_WOL_MAGIC_PACKET, 0,
			GELIC_LV1_WOL_MP_ENABLE, &v1, &v2);

		if (status) {
			dev_dbg(dev, "%s:%d: Enabling WOL failed: %d\n",
				__func__, __LINE__, status);
			status = -EIO;
			goto done;
		}

		status = lv1_net_control(bus_id(card), dev_id(card),
			GELIC_LV1_SET_WOL, GELIC_LV1_WOL_ADD_MATCH_ADDR, 0,
			GELIC_LV1_WOL_MATCH_ALL, &v1, &v2);

		if (!status) {
			ps3_sys_manager_set_wol(1);
		} else {
			dev_dbg(dev, "%s:%d: Enabling WOL filter failed: %d\n",
				__func__, __LINE__, status);
			status = -EIO;
		}
	} else {
		status = lv1_net_control(bus_id(card), dev_id(card),
			GELIC_LV1_SET_WOL, GELIC_LV1_WOL_MAGIC_PACKET,
			0, GELIC_LV1_WOL_MP_DISABLE, &v1, &v2);

		if (status) {
			dev_dbg(dev, "%s:%d: Disabling WOL failed: %d\n",
				__func__, __LINE__, status);
			status = -EIO;
			goto done;
		}

		status = lv1_net_control(bus_id(card), dev_id(card),
			GELIC_LV1_SET_WOL, GELIC_LV1_WOL_DELETE_MATCH_ADDR,
			0, GELIC_LV1_WOL_MATCH_ALL, &v1, &v2);

		if (!status) {
			ps3_sys_manager_set_wol(0);
		} else {
			dev_dbg(dev, "%s:%d: Removing WOL filter failed: %d\n",
				__func__, __LINE__, status);
			status = -EIO;
		}
	}
done:
	return status;
}

static const struct ethtool_ops gelic_ether_ethtool_ops = {
	.get_drvinfo	= gelic_net_get_drvinfo,
	.get_link	= ethtool_op_get_link,
	.get_wol	= gelic_net_get_wol,
	.set_wol	= gelic_net_set_wol,
	.get_link_ksettings = gelic_ether_get_link_ksettings,
	.set_link_ksettings = gelic_ether_set_link_ksettings,
};

static struct gelic_card *gelic_work_to_card(struct work_struct *work)
{
	return container_of(work, struct gelic_card, tx_timeout_task);
}

/**
 * gelic_net_tx_timeout_task - task scheduled by the watchdog timeout
 * function (to be called not under interrupt status)
 * @work: work is context of tx timout task
 *
 * called as task when tx hangs, resets interface (if interface is up)
 */
static void gelic_net_tx_timeout_task(struct work_struct *work)
{
	struct gelic_card *card = gelic_work_to_card(work);
	struct net_device *netdev = card->netdev[GELIC_PORT_ETHERNET_0];
	struct device *dev = ctodev(card);

	dev_info(dev, "%s:%d: Timed out. Restarting...\n", __func__, __LINE__);

	if (!(netdev->flags & IFF_UP)) {
		goto out;
	}

	netif_device_detach(netdev);
	gelic_net_stop(netdev);

	gelic_net_open(netdev);
	netif_device_attach(netdev);

out:
	atomic_dec(&card->tx_timeout_task_counter);
}

/**
 * gelic_net_tx_timeout - called when the tx timeout watchdog kicks in.
 * @netdev: interface device structure
 * @txqueue: unused
 *
 * called, if tx hangs. Schedules a task that resets the interface
 */
void gelic_net_tx_timeout(struct net_device *netdev, unsigned int txqueue)
{
	struct gelic_card *card;

	card = netdev_card(netdev);
	atomic_inc(&card->tx_timeout_task_counter);

	if (netdev->flags & IFF_UP) {
		schedule_work(&card->tx_timeout_task);
	} else {
		atomic_dec(&card->tx_timeout_task_counter);
	}
}

static const struct net_device_ops gelic_netdevice_ops = {
	.ndo_open = gelic_net_open,
	.ndo_stop = gelic_net_stop,
	.ndo_start_xmit = gelic_net_xmit,
	.ndo_set_rx_mode = gelic_net_set_multi,
	.ndo_tx_timeout = gelic_net_tx_timeout,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller = gelic_net_poll_controller,
#endif
};

/**
 * gelic_ether_setup_netdev_ops - initialization of net_device operations
 * @netdev: net_device structure
 * @napi: napi structure
 *
 * fills out function pointers in the net_device structure
 */
static void gelic_ether_setup_netdev_ops(struct net_device *netdev,
	struct napi_struct *napi)
{
	netdev->watchdog_timeo = GELIC_NET_WATCHDOG_TIMEOUT;
	/* NAPI */
	netif_napi_add(netdev, napi, gelic_net_poll, NAPI_POLL_WEIGHT);
	netdev->ethtool_ops = &gelic_ether_ethtool_ops;
	netdev->netdev_ops = &gelic_netdevice_ops;
}

/**
 * gelic_ether_setup_netdev - initialization of net_device
 * @netdev: net_device structure
 * @card: card structure
 *
 * Returns 0 on success or <0 on failure
 *
 * gelic_ether_setup_netdev initializes the net_device structure
 * and register it.
 **/
int gelic_net_setup_netdev(struct net_device *netdev, struct gelic_card *card)
{
	struct device *dev = ctodev(card);
	int status;
	u64 v1, v2;

	netdev->hw_features = NETIF_F_IP_CSUM | NETIF_F_RXCSUM;
	netdev->features = NETIF_F_IP_CSUM;

	if (GELIC_CARD_RX_CSUM_DEFAULT) {
		netdev->features |= NETIF_F_RXCSUM;
	}

	status = lv1_net_control(bus_id(card), dev_id(card),
		GELIC_LV1_GET_MAC_ADDRESS, 0, 0, 0, &v1, &v2);

	v1 <<= 16;

	if (status || !is_valid_ether_addr((u8 *)&v1)) {
		dev_dbg(dev, "%s:%d: lv1_net_control GET_MAC_ADDR failed: %d\n",
			__func__, __LINE__, status);
		return -EINVAL;
	}

	memcpy(netdev->dev_addr, &v1, ETH_ALEN);

	if (card->vlan_required) {
		netdev->hard_header_len += VLAN_HLEN;
		/*
		 * As vlan is internally used,
		 * we can not receive vlan packets
		 */
		netdev->features |= NETIF_F_VLAN_CHALLENGED;
	}

	/* MTU range: 64 - 1518 */
	netdev->min_mtu = GELIC_NET_MIN_MTU;
	netdev->max_mtu = GELIC_NET_MAX_MTU;

	status = register_netdev(netdev);

	if (status) {
		dev_err(dev, "%s:%d: Couldn't register %s: %d\n", __func__,
			__LINE__, netdev->name, status);
		return status;
	}

	dev_info(dev, "%s:%d: %s MAC addr %pxM\n", __func__, __LINE__,
		netdev->name, netdev->dev_addr);

	return 0;
}

/**
 * gelic_alloc_card_net - allocates net_device and card structure
 *
 * returns the card structure or NULL in case of errors
 *
 * the card and net_device structures are linked to each other
 */
#define GELIC_ALIGN (32)
static struct gelic_card *gelic_alloc_card_net(struct net_device **netdev)
{
	struct gelic_card *card;
	struct gelic_port *port;
	void *p;
	size_t alloc_size;
	/*
	 * gelic requires dma descriptor is 32 bytes aligned and
	 * the hypervisor requires irq_status is 8 bytes aligned.
	 */
	BUILD_BUG_ON(offsetof(struct gelic_card, irq_status) % 8);
	BUILD_BUG_ON(offsetof(struct gelic_card, descr) % 32);
	alloc_size = sizeof(struct gelic_card) +
		sizeof(struct gelic_descr) * GELIC_NET_RX_DESCRIPTORS +
		sizeof(struct gelic_descr) * GELIC_NET_TX_DESCRIPTORS +
		GELIC_ALIGN - 1;

	p  = kzalloc(alloc_size, GFP_KERNEL);

	if (!p) {
		return NULL;
	}

	card = PTR_ALIGN(p, GELIC_ALIGN);
	card->unalign = p;

	*netdev = alloc_etherdev(sizeof(struct gelic_port));

	if (!*netdev) {
		kfree(card->unalign);
		return NULL;
	}
	port = netdev_priv(*netdev);

	port->netdev = *netdev;
	port->card = card;
	port->type = GELIC_PORT_ETHERNET_0;

	card->netdev[GELIC_PORT_ETHERNET_0] = *netdev;

	INIT_WORK(&card->tx_timeout_task, gelic_net_tx_timeout_task);
	init_waitqueue_head(&card->waitq);
	atomic_set(&card->tx_timeout_task_counter, 0);
	mutex_init(&card->updown_lock);
	atomic_set(&card->users, 0);

	return card;
}

static void gelic_card_get_vlan_info(struct gelic_card *card)
{
	struct device *dev = ctodev(card);
	unsigned int i;
	u64 v1, v2;
	int status;
	struct vlan_id {
		int tx;
		int rx;
	};
	struct vlan_id vlan_id_ix[2] = {
		[GELIC_PORT_ETHERNET_0] = {
			.tx = GELIC_LV1_VLAN_TX_ETHERNET_0,
			.rx = GELIC_LV1_VLAN_RX_ETHERNET_0
		},
		[GELIC_PORT_WIRELESS] = {
			.tx = GELIC_LV1_VLAN_TX_WIRELESS,
			.rx = GELIC_LV1_VLAN_RX_WIRELESS
		}
	};

	for (i = 0; i < ARRAY_SIZE(vlan_id_ix); i++) {
		/* tx tag */
		status = lv1_net_control(bus_id(card), dev_id(card),
			GELIC_LV1_GET_VLAN_ID, vlan_id_ix[i].tx, 0, 0, &v1,
			&v2);

		if (status || !v1) {
			if (status != LV1_NO_ENTRY) {
				dev_dbg(dev,
					"%s:%d: Get vlan id for tx(%d) failed: %d\n",
					__func__, __LINE__, vlan_id_ix[i].tx,
					status);
			}
			card->vlan[i].tx = 0;
			card->vlan[i].rx = 0;
			continue;
		}
		card->vlan[i].tx = (u16)v1;

		/* rx tag */
		status = lv1_net_control(bus_id(card), dev_id(card),
			GELIC_LV1_GET_VLAN_ID, vlan_id_ix[i].rx, 0, 0, &v1,
			&v2);

		if (status || !v1) {
			if (status != LV1_NO_ENTRY) {
				dev_dbg(dev,
					"%s:%d: Get vlan id for rx(%d) failed: %d\n",
					__func__, __LINE__, vlan_id_ix[i].rx,
					status);
			}
			card->vlan[i].tx = 0;
			card->vlan[i].rx = 0;
			continue;
		}

		card->vlan[i].rx = (u16)v1;

		dev_dbg(dev, "%s:%d: vlan_id[%d] tx=%02x rx=%02x\n", __func__,
			__LINE__, i, card->vlan[i].tx, card->vlan[i].rx);
	}

	if (card->vlan[GELIC_PORT_ETHERNET_0].tx) {
		BUG_ON_DEBUG(!card->vlan[GELIC_PORT_WIRELESS].tx);
		card->vlan_required = 1;
	} else
		card->vlan_required = 0;

	/* check wirelss capable firmware */
	if (ps3_compare_firmware_version(1, 6, 0) < 0) {
		card->vlan[GELIC_PORT_WIRELESS].tx = 0;
		card->vlan[GELIC_PORT_WIRELESS].rx = 0;
	}

	dev_dbg(dev, "%s:%d: internal vlan %s\n", __func__, __LINE__,
		card->vlan_required ? "enabled" : "disabled");
}

/*
 * ps3_gelic_driver_probe - add a device to the control of this driver
 */
static int ps3_gelic_driver_probe(struct ps3_system_bus_device *sb_dev)
{
	struct device *dev = &sb_dev->core;
	struct gelic_card *card;
	struct net_device *netdev;
	int result;

	dev_dbg(dev, "%s:%d: >\n", __func__, __LINE__);

	udbg_shutdown_ps3gelic();

	result = ps3_open_hv_device(sb_dev);

	if (result) {
		dev_err(dev, "%s:%d: ps3_open_hv_device failed: %d\n",
			__func__, __LINE__, result);
		goto fail_open;
	}

	result = ps3_dma_region_create(sb_dev->d_region);

	if (result) {
		dev_err(dev, "%s:%d: ps3_dma_region_create failed: %d\n",
			__func__, __LINE__, result);
		BUG_ON_DEBUG("check region type");
		goto fail_dma_region;
	}

	card = gelic_alloc_card_net(&netdev);

	if (!card) {
		dev_info(dev, "%s:%d: gelic_net_alloc_card failed.\n", __func__,
			__LINE__);
		result = -ENOMEM;
		goto fail_alloc_card;
	}

	ps3_system_bus_set_drvdata(sb_dev, card);
	card->dev = sb_dev;

	gelic_card_get_vlan_info(card);

	card->link_mode = GELIC_LV1_ETHER_AUTO_NEG;

	result = lv1_net_set_interrupt_status_indicator(bus_id(card),
		dev_id(card), ps3_mm_phys_to_lpar(__pa(&card->irq_status)), 0);

	if (result) {
		dev_dbg(dev,
			"%s:%d: set_interrupt_status_indicator failed: %s\n",
			__func__, __LINE__, ps3_result(result));
		result = -EIO;
		goto fail_status_indicator;
	}

	result = ps3_sb_event_receive_port_setup(sb_dev, PS3_BINDING_CPU_ANY,
		&card->irq);

	if (result) {
		dev_dbg(dev, "%s:%d: gelic_net_open_device failed: %d\n",
			__func__, __LINE__, result);
		result = -EPERM;
		goto fail_alloc_irq;
	}

	result = request_irq(card->irq, gelic_card_interrupt, 0, netdev->name,
		card);

	if (result) {
		dev_dbg(dev, "%s:%d: request_irq failed: %d\n",
			__func__, __LINE__, result);
		goto fail_request_irq;
	}

	card->irq_mask = GELIC_CARD_RXINT | GELIC_CARD_TXINT |
		GELIC_CARD_PORT_STATUS_CHANGED;

	result = gelic_card_init_chain(card, &card->tx_chain, card->descr,
		GELIC_NET_TX_DESCRIPTORS);

	if (result) {
		goto fail_alloc_tx;
	}

	result = gelic_card_init_chain(card, &card->rx_chain,
		card->descr + GELIC_NET_TX_DESCRIPTORS,
		GELIC_NET_RX_DESCRIPTORS);

	if (result) {
		goto fail_alloc_rx;
	}

	card->tx_top = card->tx_chain.head;
	card->rx_top = card->rx_chain.head;

	dev_dbg(dev, "%s:%d: descr rx %px, tx %px, size %#lx, num %#x\n",
		__func__, __LINE__, card->rx_top, card->tx_top,
		sizeof(struct gelic_descr), GELIC_NET_RX_DESCRIPTORS);

	result = gelic_card_alloc_rx_skbs(card);

	if (result) {
		goto fail_alloc_skbs;
	}

	spin_lock_init(&card->tx_lock);
	card->tx_dma_progress = 0;

	netdev->irq = card->irq;
	SET_NETDEV_DEV(netdev, dev);
	gelic_ether_setup_netdev_ops(netdev, &card->napi);

	result = gelic_net_setup_netdev(netdev, card);

	if (result) {
		dev_err(dev, "%s:%d: setup_netdev failed: %d\n", __func__,
			__LINE__, result);
		goto fail_setup_netdev;
	}

#ifdef CONFIG_GELIC_WIRELESS
	result = gelic_wl_driver_probe(card);

	if (result) {
		dev_dbg(dev, "%s:%d: WL init failed\n", __func__, __LINE__);
		goto fail_setup_netdev;
	}
#endif
	dev_dbg(dev, "%s:%d: < OK\n", __func__, __LINE__);
	return 0;

fail_setup_netdev:
fail_alloc_skbs:
	gelic_card_free_chain(card, card->rx_chain.head);
fail_alloc_rx:
	gelic_card_free_chain(card, card->tx_chain.head);
fail_alloc_tx:
	free_irq(card->irq, card);
	netdev->irq = 0;
fail_request_irq:
	ps3_sb_event_receive_port_destroy(sb_dev, card->irq);
fail_alloc_irq:
	lv1_net_set_interrupt_status_indicator(bus_id(card), bus_id(card), 0,
		0);
fail_status_indicator:
	ps3_system_bus_set_drvdata(sb_dev, NULL);
	kfree(netdev_card(netdev)->unalign);
	free_netdev(netdev);
fail_alloc_card:
	ps3_dma_region_free(sb_dev->d_region);
fail_dma_region:
	ps3_close_hv_device(sb_dev);
fail_open:
	dev_dbg(dev, "%s:%d: < error\n", __func__, __LINE__);
	return result;
}

/*
 * ps3_gelic_driver_remove - remove a device from the control of this driver
 */

static void ps3_gelic_driver_remove(struct ps3_system_bus_device *sb_dev)
{
	struct gelic_card *card = ps3_system_bus_get_drvdata(sb_dev);
	struct device *dev = &sb_dev->core;
	struct net_device *netdev0;

	dev_dbg(dev, "%s:%d: >\n", __func__, __LINE__);

	gelic_card_set_link_mode(card, GELIC_LV1_ETHER_AUTO_NEG);

#ifdef CONFIG_GELIC_WIRELESS
	gelic_wl_driver_remove(card);
#endif
	gelic_card_set_irq_mask(card, 0);

	gelic_card_disable_rxdmac(card);
	gelic_card_disable_txdmac(card);

	gelic_card_release_tx_chain(card, 1);
	gelic_card_release_rx_chain(card);

	gelic_card_free_chain(card, card->tx_top);
	gelic_card_free_chain(card, card->rx_top);

	netdev0 = card->netdev[GELIC_PORT_ETHERNET_0];

	free_irq(card->irq, card);
	netdev0->irq = 0;
	ps3_sb_event_receive_port_destroy(card->dev, card->irq);

	wait_event(card->waitq,
		atomic_read(&card->tx_timeout_task_counter) == 0);

	lv1_net_set_interrupt_status_indicator(bus_id(card), dev_id(card), 0,
		0);

	unregister_netdev(netdev0);
	kfree(netdev_card(netdev0)->unalign);
	free_netdev(netdev0);

	ps3_system_bus_set_drvdata(sb_dev, NULL);

	ps3_dma_region_free(sb_dev->d_region);

	ps3_close_hv_device(sb_dev);

	dev_dbg(dev, "%s:%d: <\n", __func__, __LINE__);
}

static struct ps3_system_bus_driver ps3_gelic_driver = {
	.match_id = PS3_MATCH_ID_GELIC,
	.probe = ps3_gelic_driver_probe,
	.remove = ps3_gelic_driver_remove,
	.shutdown = ps3_gelic_driver_remove,
	.core.name = "ps3_gelic_driver",
	.core.owner = THIS_MODULE,
};

static int __init ps3_gelic_driver_init(void)
{
	return firmware_has_feature(FW_FEATURE_PS3_LV1)
		? ps3_system_bus_driver_register(&ps3_gelic_driver)
		: -ENODEV;
}

static void __exit ps3_gelic_driver_exit(void)
{
	ps3_system_bus_driver_unregister(&ps3_gelic_driver);
}

module_init(ps3_gelic_driver_init);
module_exit(ps3_gelic_driver_exit);

MODULE_ALIAS(PS3_MODULE_ALIAS_GELIC);
