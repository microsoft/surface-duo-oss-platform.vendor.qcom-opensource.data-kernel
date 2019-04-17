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
#include <linux/iommu.h>
#include <linux/msm_gsi.h>

#include "aqo_i.h"

int aqo_gsi_init_rx(struct aqo_device *aqo_dev)
{
	int rc = 0;
	u64 head_ptr;
	enum aqo_proxy_mode proxy_mode = aqo_dev->ch_rx.proxy.mode;
	struct ipa_eth_channel *ch = AQO_ETHDEV(aqo_dev)->ch_rx;

	struct gsi_evt_ring_props gsi_evt_ring_props;

	struct gsi_chan_props gsi_channel_props;
	union gsi_channel_scratch ch_scratch;

	memset(&gsi_evt_ring_props, 0, sizeof(gsi_evt_ring_props));

	gsi_evt_ring_props.intf = GSI_EVT_CHTYPE_AQC_EV;
	gsi_evt_ring_props.intr = GSI_INTR_MSI;
	gsi_evt_ring_props.re_size = GSI_EVT_RING_RE_SIZE_16B;

	gsi_evt_ring_props.ring_len = ch->desc_mem.size;
	gsi_evt_ring_props.ring_base_addr = ch->desc_mem.daddr;
	gsi_evt_ring_props.ring_base_vaddr = NULL;

	gsi_evt_ring_props.int_modt = aqo_dev->ch_rx.gsi_modt;
	gsi_evt_ring_props.int_modc = aqo_dev->ch_rx.gsi_modc;

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

	gsi_evt_ring_props.exclusive = true;
	gsi_evt_ring_props.err_cb = NULL;
	gsi_evt_ring_props.user_data = ch;

	memset(&gsi_channel_props, 0, sizeof(gsi_channel_props));

	gsi_channel_props.prot = GSI_CHAN_PROT_AQC;
	gsi_channel_props.dir = GSI_CHAN_DIR_TO_GSI;

	gsi_channel_props.re_size = GSI_CHAN_RE_SIZE_16B;
	gsi_channel_props.ring_len = ch->desc_mem.size;
	gsi_channel_props.max_re_expected = 0;

	/* Use virtual address since the physical pages may be scattered */
	rc = ipa_eth_gsi_iommu_vamap(ch->desc_mem.daddr, ch->desc_mem.vaddr,
				     ch->desc_mem.size,
				     IOMMU_READ | IOMMU_WRITE, true);
	if (rc) {
		aqo_log_err(aqo_dev,
			"Failed to map AQC Rx descriptor memory to IPA");
		goto err_map_desc;
	}

	aqo_log(aqo_dev,
		"Mapped %u bytes of AQC Rx descriptor memory at VA %p to DA %p",
		ch->desc_mem.size, ch->desc_mem.vaddr, ch->desc_mem.daddr);

	gsi_channel_props.ring_base_addr = ch->desc_mem.daddr;
	gsi_channel_props.ring_base_vaddr = NULL;

	if (proxy_mode == AQO_PROXY_MODE_HEADPTR)
		gsi_channel_props.use_db_eng = GSI_CHAN_DIRECT_MODE;
	else
		gsi_channel_props.use_db_eng = GSI_CHAN_DB_MODE;

	gsi_channel_props.max_prefetch = GSI_ONE_PREFETCH_SEG;
	gsi_channel_props.low_weight = 1;

	gsi_channel_props.xfer_cb = NULL;
	gsi_channel_props.err_cb = NULL;
	gsi_channel_props.chan_user_data = ch;

	rc = ipa_eth_gsi_iommu_pamap(ch->buff_mem.daddr, ch->buff_mem.paddr,
				     ch->buff_mem.size,
				     IOMMU_READ | IOMMU_WRITE, false);
	if (rc) {
		aqo_log_err(aqo_dev,
			"Failed to map AQC Rx buffer memory to IPA");
		goto err_map_buff;
	}

	aqo_log(aqo_dev,
		"Mapped %u bytes of AQC Rx buffer memory at PA %p to DA %p",
		ch->desc_mem.size, ch->desc_mem.paddr, ch->desc_mem.daddr);

	memset(&ch_scratch, 0, sizeof(ch_scratch));

	ch_scratch.data.word1 = lower_32_bits(ch->buff_mem.daddr);

	if (upper_32_bits(ch->buff_mem.daddr) & ~(BIT(8) - 1))
		aqo_log_bug(aqo_dev,
			"Excess Rx buffer memory address bits will be ignored");

	ch_scratch.data.word2 = upper_32_bits(ch->buff_mem.daddr);
	ch_scratch.data.word2 &= (BIT(8) - 1);
	ch_scratch.data.word2 |= (ilog2(ch->buff_size) << 16);

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
	ipa_eth_gsi_iommu_unmap(ch->buff_mem.daddr, ch->buff_mem.size, false);
err_map_buff:
	ipa_eth_gsi_iommu_unmap(ch->desc_mem.daddr, ch->desc_mem.size, true);
err_map_desc:
	return rc;
}

int aqo_gsi_deinit_rx(struct aqo_device *aqo_dev)
{
	struct ipa_eth_channel *ch = AQO_ETHDEV(aqo_dev)->ch_rx;

	// TODO: check return value
	ipa_eth_gsi_dealloc(ch);
	ipa_eth_gsi_iommu_unmap(ch->buff_mem.daddr, ch->buff_mem.size, false);
	ipa_eth_gsi_iommu_unmap(ch->desc_mem.daddr, ch->desc_mem.size, true);

	return 0;
}

int aqo_gsi_start_rx(struct aqo_device *aqo_dev)
{
	return ipa_eth_gsi_start(AQO_ETHDEV(aqo_dev)->ch_rx);
}

int aqo_gsi_stop_rx(struct aqo_device *aqo_dev)
{
	return ipa_eth_gsi_stop(AQO_ETHDEV(aqo_dev)->ch_rx);
}

int aqo_gsi_init_tx(struct aqo_device *aqo_dev)
{
	int rc = 0;

	struct ipa_eth_channel *ch = AQO_ETHDEV(aqo_dev)->ch_tx;

	struct gsi_evt_ring_props gsi_evt_ring_props;

	struct gsi_chan_props gsi_channel_props;
	union gsi_channel_scratch ch_scratch;

	memset(&gsi_evt_ring_props, 0, sizeof(gsi_evt_ring_props));

	gsi_evt_ring_props.intf = GSI_EVT_CHTYPE_AQC_EV;
	gsi_evt_ring_props.intr = GSI_INTR_MSI;
	gsi_evt_ring_props.re_size = GSI_EVT_RING_RE_SIZE_16B;

	gsi_evt_ring_props.ring_len = ch->desc_mem.size;
	gsi_evt_ring_props.ring_base_addr = ch->desc_mem.daddr;
	gsi_evt_ring_props.ring_base_vaddr = NULL;

	gsi_evt_ring_props.int_modt = aqo_dev->ch_tx.gsi_modt;
	gsi_evt_ring_props.int_modc = aqo_dev->ch_tx.gsi_modc;

	if (aqo_dev->pci_direct) {
		gsi_evt_ring_props.msi_addr =
			AQC_TX_TAIL_PTR(aqo_dev->regs_base.paddr,  ch->queue);
		gsi_evt_ring_props.msi_addr =
			AQO_PCI_DIRECT_SET(gsi_evt_ring_props.msi_addr);
	} else {
		aqo_log_bug(aqo_dev, "Only PCI direct access is supported");
		return -EFAULT;
	}

	gsi_evt_ring_props.exclusive = true;
	gsi_evt_ring_props.err_cb = NULL;
	gsi_evt_ring_props.user_data = ch;

	memset(&gsi_channel_props, 0, sizeof(gsi_channel_props));

	gsi_channel_props.prot = GSI_CHAN_PROT_AQC;
	gsi_channel_props.dir = GSI_CHAN_DIR_FROM_GSI;

	gsi_channel_props.re_size = GSI_CHAN_RE_SIZE_16B;
	gsi_channel_props.ring_len = ch->desc_mem.size;
	gsi_channel_props.max_re_expected = 0;

	/* Use virtual address since the physical pages may be scattered */
	rc = ipa_eth_gsi_iommu_vamap(ch->desc_mem.daddr, ch->desc_mem.vaddr,
				     ch->desc_mem.size,
				     IOMMU_READ | IOMMU_WRITE, true);
	if (rc) {
		aqo_log_err(aqo_dev,
			"Failed to map AQC Tx descriptor memory to IPA");
		goto err_map_desc;
	}

	aqo_log(aqo_dev,
		"Mapped %u bytes of AQC Tx descriptor memory at VA %p to DA %p",
		ch->desc_mem.size, ch->desc_mem.vaddr, ch->desc_mem.daddr);

	gsi_channel_props.ring_base_addr = ch->desc_mem.daddr;
	gsi_channel_props.ring_base_vaddr = NULL;

	gsi_channel_props.use_db_eng = GSI_CHAN_DB_MODE;

	gsi_channel_props.max_prefetch = GSI_ONE_PREFETCH_SEG;
	gsi_channel_props.low_weight = 1;

	gsi_channel_props.xfer_cb = NULL;
	gsi_channel_props.err_cb = NULL;
	gsi_channel_props.chan_user_data = ch;

	rc = ipa_eth_gsi_iommu_pamap(ch->buff_mem.daddr, ch->buff_mem.paddr,
				     ch->buff_mem.size,
				     IOMMU_READ | IOMMU_WRITE, false);
	if (rc) {
		aqo_log_err(aqo_dev,
			"Failed to map AQC Tx buffer memory to IPA");
		goto err_map_buff;
	}

	aqo_log(aqo_dev,
		"Mapped %u bytes of AQC Tx buffer memory at PA %p to DA %p",
		ch->desc_mem.size, ch->desc_mem.paddr, ch->desc_mem.daddr);

	memset(&ch_scratch, 0, sizeof(ch_scratch));

	ch_scratch.data.word2 |= (ilog2(ch->buff_size) << 16);

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
	ipa_eth_gsi_iommu_unmap(ch->buff_mem.daddr, ch->buff_mem.size, false);
err_map_buff:
	ipa_eth_gsi_iommu_unmap(ch->desc_mem.daddr, ch->desc_mem.size, true);
err_map_desc:
	return rc;
}

int aqo_gsi_deinit_tx(struct aqo_device *aqo_dev)
{
	struct ipa_eth_channel *ch = AQO_ETHDEV(aqo_dev)->ch_tx;

	// TODO: check return value
	ipa_eth_gsi_dealloc(ch);
	ipa_eth_gsi_iommu_unmap(ch->buff_mem.daddr, ch->buff_mem.size, false);
	ipa_eth_gsi_iommu_unmap(ch->desc_mem.daddr, ch->desc_mem.size, true);

	return 0;
}

int aqo_gsi_start_tx(struct aqo_device *aqo_dev)
{
	int rc;
	struct ipa_eth_channel *ch = AQO_ETHDEV(aqo_dev)->ch_tx;
	u32 chdb_val = AQO_ETHDEV(aqo_dev)->ch_tx->desc_count - 1;

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
	return ipa_eth_gsi_stop(AQO_ETHDEV(aqo_dev)->ch_tx);
}
