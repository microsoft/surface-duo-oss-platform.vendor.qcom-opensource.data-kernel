/* Copyright (c) 2019 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/log2.h>
#include <linux/msm_gsi.h>

#include "aqo_i.h"

static void aqo_gsi_init_ev_ring_props(struct gsi_evt_ring_props *props,
		const struct ipa_eth_resource *mem)
{
	memset(props, 0, sizeof(*props));

	props->intf = GSI_EVT_CHTYPE_AQC_EV;
	props->intr = GSI_INTR_MSI;
	props->re_size = GSI_EVT_RING_RE_SIZE_16B;

	props->ring_len = mem->size;
	props->ring_base_addr = mem->daddr;

	props->exclusive = true;
}

static void aqo_gsi_init_ch_props(struct gsi_chan_props *props,
		const struct ipa_eth_resource *mem, enum gsi_chan_dir dir)
{
	memset(props, 0, sizeof(*props));

	props->prot = GSI_CHAN_PROT_AQC;
	props->dir = dir;
	props->re_size = GSI_CHAN_RE_SIZE_16B;

	props->ring_len = mem->size;
	props->ring_base_addr = mem->daddr;
}

int aqo_gsi_init_rx(struct aqo_device *aqo_dev)
{
	int rc = 0;
	u64 head_ptr;
	enum aqo_proxy_mode proxy_mode = aqo_dev->ch_rx.proxy.mode;
	struct ipa_eth_channel *ch = aqo_dev->ch_rx.eth_ch;
	struct ipa_eth_channel_mem *desc = list_first_entry(
		&ch->desc_mem, struct ipa_eth_channel_mem, mem_list_entry);
	struct ipa_eth_resource *desc_cb_mem = ipa_eth_net_ch_to_cb_mem(ch,
							desc, IPA_ETH_HW_GSI);
	struct ipa_eth_channel_mem *buff = list_first_entry(
		&ch->buff_mem, struct ipa_eth_channel_mem, mem_list_entry);
	struct ipa_eth_resource *buff_cb_mem = ipa_eth_net_ch_to_cb_mem(ch,
							buff, IPA_ETH_HW_IPA);
	struct gsi_evt_ring_props gsi_evt_ring_props;
	struct gsi_chan_props gsi_channel_props;
	union gsi_channel_scratch ch_scratch;

	if (!desc_cb_mem || !buff_cb_mem) {
		aqo_log_err(aqo_dev, "Failed to get cb mem information");
		return -EFAULT;
	}

	if (buff->mem.daddr != buff_cb_mem->daddr) {
		aqo_log_err(aqo_dev, "Buffers should be mapped symmetrically");
		return -EFAULT;
	}

	/* Set GSI event ring props */

	aqo_gsi_init_ev_ring_props(&gsi_evt_ring_props, &desc->mem);

	if (aqo_dev->pci_direct) {
		gsi_evt_ring_props.msi_addr =
			AQC_RX_TAIL_PTR(aqo_dev->regs_base.paddr, ch->queue);
		gsi_evt_ring_props.msi_addr =
			AQO_PCI_DIRECT_SET(gsi_evt_ring_props.msi_addr);

		head_ptr = AQC_RX_HEAD_PTR(aqo_dev->regs_base.paddr, ch->queue);
		head_ptr = AQO_PCI_DIRECT_SET(head_ptr);
	} else {
		aqo_log_bug(aqo_dev, "Only PCI direct access is supported");
		return -EFAULT;
	}

	gsi_evt_ring_props.int_modt = aqo_dev->ch_rx.gsi_modt;
	gsi_evt_ring_props.int_modc = aqo_dev->ch_rx.gsi_modc;

	/* Set GSI transfer ring props */

	aqo_gsi_init_ch_props(&gsi_channel_props,
			desc_cb_mem, GSI_CHAN_DIR_TO_GSI);

	if (proxy_mode == AQO_PROXY_MODE_HEADPTR)
		gsi_channel_props.use_db_eng = GSI_CHAN_DIRECT_MODE;
	else
		gsi_channel_props.use_db_eng = GSI_CHAN_DB_MODE;

	gsi_channel_props.max_prefetch = GSI_ONE_PREFETCH_SEG;
	gsi_channel_props.low_weight = 1;

	/* Set GSI channel scratch */

	memset(&ch_scratch, 0, sizeof(ch_scratch));

	ch_scratch.data.word1 = lower_32_bits(buff->mem.daddr);

	if (upper_32_bits(buff->mem.daddr) & ~(BIT(8) - 1))
		aqo_log_bug(aqo_dev,
			"Excess Rx buffer memory address bits will be ignored");

	ch_scratch.data.word2 = upper_32_bits(buff->mem.daddr);
	ch_scratch.data.word2 &= (BIT(8) - 1);
	ch_scratch.data.word2 |= (ilog2(ch->mem_params.buff.size) << 16);

	if (gsi_channel_props.use_db_eng == GSI_CHAN_DB_MODE) {
		ch_scratch.data.word3 = lower_32_bits(head_ptr);
		ch_scratch.data.word4 = upper_32_bits(head_ptr) & (BIT(9) - 1);

		if (upper_32_bits(head_ptr) & ~(BIT(9) - 1))
			aqo_log_bug(aqo_dev,
				"Excess Head Pointer address bits are ignored");
	}

	rc = ipa_eth_gsi_alloc(ch, &gsi_evt_ring_props, NULL, NULL,
			&gsi_channel_props, &ch_scratch,
			&aqo_dev->ch_rx.gsi_db.paddr);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to alloc Rx GSI rings");
		goto err_gsi_alloc;
	}

	aqo_dev->ch_rx.gsi_ch = gsi_channel_props.ch_id;

	rc = ipa_eth_gsi_ring_evtring(ch, gsi_evt_ring_props.ring_base_addr +
					gsi_evt_ring_props.ring_len);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to ring Rx GSI event ring");
		goto err_ring_ev;
	}

	return 0;

err_ring_ev:
	ipa_eth_gsi_dealloc(ch);
err_gsi_alloc:
	return rc;
}

int aqo_gsi_deinit_rx(struct aqo_device *aqo_dev)
{
	struct ipa_eth_channel *ch = aqo_dev->ch_rx.eth_ch;

	// TODO: check return value
	ipa_eth_gsi_dealloc(ch);

	return 0;
}

int aqo_gsi_start_rx(struct aqo_device *aqo_dev)
{
	return ipa_eth_gsi_start(aqo_dev->ch_rx.eth_ch);
}

int aqo_gsi_stop_rx(struct aqo_device *aqo_dev)
{
	return ipa_eth_gsi_stop(aqo_dev->ch_rx.eth_ch);
}

int aqo_gsi_init_tx(struct aqo_device *aqo_dev)
{
	int rc = 0;
	struct ipa_eth_channel *ch = aqo_dev->ch_tx.eth_ch;
	struct ipa_eth_channel_mem *desc = list_first_entry(
		&ch->desc_mem, struct ipa_eth_channel_mem, mem_list_entry);
	struct ipa_eth_resource *desc_cb_mem = ipa_eth_net_ch_to_cb_mem(ch,
							desc, IPA_ETH_HW_GSI);
	struct ipa_eth_channel_mem *buff = list_first_entry(
		&ch->buff_mem, struct ipa_eth_channel_mem, mem_list_entry);
	struct ipa_eth_resource *buff_cb_mem = ipa_eth_net_ch_to_cb_mem(ch,
							buff, IPA_ETH_HW_IPA);
	struct gsi_evt_ring_props gsi_evt_ring_props;
	struct gsi_chan_props gsi_channel_props;
	union gsi_channel_scratch ch_scratch;

	if (!desc_cb_mem || !buff_cb_mem) {
		aqo_log_err(aqo_dev, "Failed to get cb mem information");
		return -EFAULT;
	}

	if (buff->mem.daddr != buff_cb_mem->daddr) {
		aqo_log_err(aqo_dev, "Buffers should be mapped symmetrically");
		return -EFAULT;
	}

	/* Set GSI event ring props */

	aqo_gsi_init_ev_ring_props(&gsi_evt_ring_props, &desc->mem);

	if (aqo_dev->pci_direct) {
		gsi_evt_ring_props.msi_addr =
			AQC_TX_TAIL_PTR(aqo_dev->regs_base.paddr, ch->queue);
		gsi_evt_ring_props.msi_addr =
			AQO_PCI_DIRECT_SET(gsi_evt_ring_props.msi_addr);
	} else {
		aqo_log_bug(aqo_dev, "Only PCI direct access is supported");
		return -EFAULT;
	}

	gsi_evt_ring_props.int_modt = aqo_dev->ch_tx.gsi_modt;
	gsi_evt_ring_props.int_modc = aqo_dev->ch_tx.gsi_modc;

	/* Set GSI transfer ring props */

	aqo_gsi_init_ch_props(&gsi_channel_props,
			desc_cb_mem, GSI_CHAN_DIR_FROM_GSI);

	gsi_channel_props.use_db_eng = GSI_CHAN_DB_MODE;

	gsi_channel_props.max_prefetch = GSI_ONE_PREFETCH_SEG;
	gsi_channel_props.low_weight = 1;

	/* Set GSI channel scratch */

	memset(&ch_scratch, 0, sizeof(ch_scratch));

	ch_scratch.data.word1 = aqo_dev->tx_wrb_mod_count;
	ch_scratch.data.word2 |= (ilog2(ch->mem_params.buff.size) << 16);

	rc = ipa_eth_gsi_alloc(ch, &gsi_evt_ring_props, NULL, NULL,
			&gsi_channel_props, &ch_scratch,
			&aqo_dev->ch_tx.gsi_db.paddr);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to alloc Tx GSI rings");
		goto err_gsi_alloc;
	}

	aqo_dev->ch_tx.gsi_ch = gsi_channel_props.ch_id;

	rc = ipa_eth_gsi_ring_evtring(ch, gsi_evt_ring_props.ring_base_addr +
					gsi_evt_ring_props.ring_len);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to ring Tx GSI event ring");
		goto err_ring_ev;
	}

	return 0;

err_ring_ev:
	ipa_eth_gsi_dealloc(ch);
err_gsi_alloc:
	return rc;
}

int aqo_gsi_deinit_tx(struct aqo_device *aqo_dev)
{
	struct ipa_eth_channel *ch = aqo_dev->ch_tx.eth_ch;

	// TODO: check return value
	ipa_eth_gsi_dealloc(ch);

	return 0;
}

int aqo_gsi_start_tx(struct aqo_device *aqo_dev)
{
	int rc;
	struct ipa_eth_channel *ch = aqo_dev->ch_tx.eth_ch;
	u32 chdb_val = ch->mem_params.desc.count - 1;

	rc = ipa_eth_gsi_start(ch);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to start Tx GSI rings");
		return rc;
	}

	rc = ipa_eth_gsi_ring_channel(ch, chdb_val);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to ring Tx GSI channel");
		ipa_eth_gsi_stop(ch);
		return rc;
	}

	return 0;
}

int aqo_gsi_stop_tx(struct aqo_device *aqo_dev)
{
	return ipa_eth_gsi_stop(aqo_dev->ch_tx.eth_ch);
}
