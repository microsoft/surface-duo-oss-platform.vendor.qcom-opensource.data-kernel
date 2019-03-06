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

	gsi_evt_ring_props.int_modt = aqo_dev->ch_rx.gsi_modt; // FIXME:
	gsi_evt_ring_props.int_modc = aqo_dev->ch_rx.gsi_modc; // FIXME:

	if (aqo_dev->pci_direct) {
		gsi_evt_ring_props.msi_addr =
			AQC_RX_TAIL_PTR(aqo_dev->regs_base.paddr, ch->queue);
		gsi_evt_ring_props.msi_addr =
			AQO_PCI_DIRECT_SET(gsi_evt_ring_props.msi_addr);
	} else {
		// TODO: dma_map_resource - ipa_eth_dma_map_resource()
		pr_crit("AQC: NON PCI DIRECT UNSUPPORTED");
	}

	gsi_evt_ring_props.exclusive = true;
	gsi_evt_ring_props.err_cb = NULL; // FIXME: do we need this? see enum gsi_evt_err
	gsi_evt_ring_props.user_data = ch; // FIXME: needed by err_cb

	// Unused:
	//	gsi_evt_ring_props.rp_update_addr
	// 	gsi_evt_ring_props.intvec
	// 	gsi_evt_ring_props.evchid_valid
	// 	gsi_evt_ring_props.evchid

	memset(&gsi_channel_props, 0, sizeof(gsi_channel_props));

	gsi_channel_props.prot = GSI_CHAN_PROT_AQC;
	gsi_channel_props.dir = GSI_CHAN_DIR_TO_GSI;

#if 0
	gsi_channel_props.ch_id = ; // set by ipa_eth
	gsi_channel_props.evt_ring_hdl = ; // set by ipa_eth
#endif

	gsi_channel_props.re_size = GSI_CHAN_RE_SIZE_16B;
	gsi_channel_props.ring_len = ch->desc_mem.size;
	gsi_channel_props.max_re_expected = 0;

	rc = ipa_eth_gsi_iommu_vamap(ch->desc_mem.daddr, ch->desc_mem.vaddr,
				     ch->desc_mem.size,
				     IOMMU_READ | IOMMU_WRITE, true);
	if (rc) {
		pr_crit("AQC: FAILED TO MAP RX DESC MEM");
		return rc;
	}

	pr_crit("AQC: Rx: desc: paddr = %p, daddr = %p", ch->desc_mem.paddr, ch->desc_mem.daddr);

	gsi_channel_props.ring_base_addr = ch->desc_mem.daddr;
	gsi_channel_props.ring_base_vaddr = NULL;

	pr_crit("AQC: %s: gsi_channel_props.ring_base_addr=%x", __func__, gsi_channel_props.ring_base_addr);
	pr_crit("AQC: %s: gsi_channel_props.ring_len=%x", __func__, gsi_channel_props.ring_len);

	gsi_channel_props.use_db_eng = GSI_CHAN_DIRECT_MODE; // TODO: double check

	gsi_channel_props.max_prefetch = GSI_ONE_PREFETCH_SEG; // TODO: double check
	gsi_channel_props.low_weight = 1; // TODO: double check

#if 0
	gsi_channel_props.prefetch_mode = ; // set by ipa eth
	gsi_channel_props.empty_lvl_threshold = ;  // set by ipa eth
#endif

	gsi_channel_props.xfer_cb = NULL; // FIXME: do we need this?
	gsi_channel_props.err_cb = NULL; // FIXME: have a handler to log error
	gsi_channel_props.chan_user_data = ch; // FIXME: do we need this?

	memset(&ch_scratch, 0, sizeof(ch_scratch));

	rc = ipa_eth_gsi_iommu_pamap(ch->buff_mem.daddr, ch->buff_mem.paddr,
				     ch->buff_mem.size,
				     IOMMU_READ | IOMMU_WRITE, false);
	if (rc) {
		pr_crit("AQC: FAILED TO MAP RX BUFFER MEM");
		return rc;
	}

	pr_crit("AQC: Rx: buff: paddr = %p, daddr = %p", ch->buff_mem.paddr, ch->buff_mem.daddr);

	ch_scratch.data.word1 = lower_32_bits(ch->buff_mem.daddr);

	if (upper_32_bits(ch->buff_mem.daddr) & ~(BIT(8) - 1))
		pr_crit("AQC: BUFFER MEM UPPER BITS EXCEEDED");

	ch_scratch.data.word2 = upper_32_bits(ch->buff_mem.daddr);
	ch_scratch.data.word2 |= (ilog2(2048) << 16);

	pr_crit("AQC: %s: ch_scratch.data.word1=%x", __func__, ch_scratch.data.word1);
	pr_crit("AQC: %s: ch_scratch.data.word2=%x", __func__, ch_scratch.data.word2);

	rc = ipa_eth_gsi_alloc(ch, &gsi_evt_ring_props, NULL, NULL,
			&gsi_channel_props, &ch_scratch,
			&aqo_dev->ch_rx.gsi_db.paddr);
	if (rc) {
		pr_crit("AQC: failed to alloc gsi");
		return rc;
	}

	aqo_dev->ch_rx.gsi_ch = gsi_channel_props.ch_id;

	rc = ipa_eth_gsi_ring_evtring(ch,
		gsi_evt_ring_props.ring_base_addr + gsi_evt_ring_props.ring_len);
	if (rc) {
		pr_crit("AQC: failed to ring evt ring db");
		goto err_ring_ev;
	}

	return 0;

err_ring_ev:
	ipa_eth_gsi_dealloc(ch);

	return rc;
}

int aqo_gsi_deinit_rx(struct aqo_device *aqo_dev)
{
	struct ipa_eth_channel *ch = AQO_ETHDEV(aqo_dev)->ch_rx;

	// TODO: check return value
	ipa_eth_gsi_ring_evtring(ch, 0);
	ipa_eth_gsi_dealloc(ch);

	// FIXME: Unmap addresses from SMMU

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

	gsi_evt_ring_props.int_modt = aqo_dev->ch_tx.gsi_modt; // FIXME:
	gsi_evt_ring_props.int_modc = aqo_dev->ch_tx.gsi_modc; // FIXME:

	if (aqo_dev->pci_direct) {
		gsi_evt_ring_props.msi_addr =
			AQC_TX_TAIL_PTR(aqo_dev->regs_base.paddr,  ch->queue);
		gsi_evt_ring_props.msi_addr =
			AQO_PCI_DIRECT_SET(gsi_evt_ring_props.msi_addr);
	} else {
		// TODO: dma_map_resource - ipa_eth_dma_map_resource()
		pr_crit("AQC: NON PCI DIRECT UNSUPPORTED");
	}

	gsi_evt_ring_props.exclusive = true;
	gsi_evt_ring_props.err_cb = NULL; // FIXME: do we need this? see enum gsi_evt_err
	gsi_evt_ring_props.user_data = ch; // FIXME: needed by err_cb

	// Unused:
	// 	gsi_evt_ring_props.intvec
	// 	gsi_evt_ring_props.rp_update_addr
	// 	gsi_evt_ring_props.evchid_valid
	// 	gsi_evt_ring_props.evchid

	memset(&gsi_channel_props, 0, sizeof(gsi_channel_props));

	gsi_channel_props.prot = GSI_CHAN_PROT_AQC;
	gsi_channel_props.dir = GSI_CHAN_DIR_FROM_GSI;

#if 0
	gsi_channel_props.ch_id = ; // set by ipa_eth
	gsi_channel_props.evt_ring_hdl = ; // set by ipa_eth
#endif

	gsi_channel_props.re_size = GSI_CHAN_RE_SIZE_16B;
	gsi_channel_props.ring_len = ch->desc_mem.size;
	gsi_channel_props.max_re_expected = 0;


	rc = ipa_eth_gsi_iommu_vamap(ch->desc_mem.daddr, ch->desc_mem.vaddr,
				     ch->desc_mem.size,
				     IOMMU_READ | IOMMU_WRITE, true);
	if (rc) {
		pr_crit("AQC: FAILED TO MAP TX DESC MEM");
		return rc;
	}

	pr_crit("AQC: Tx: desc: paddr = %p, daddr = %p", ch->desc_mem.paddr, ch->desc_mem.daddr);

	gsi_channel_props.ring_base_addr = ch->desc_mem.daddr;
	gsi_channel_props.ring_base_vaddr = NULL;

	pr_crit("AQC: %s: gsi_channel_props.ring_base_addr=%x", __func__, gsi_channel_props.ring_base_addr);
	pr_crit("AQC: %s: gsi_channel_props.ring_len=%x", __func__, gsi_channel_props.ring_len);

	gsi_channel_props.use_db_eng = GSI_CHAN_DB_MODE; // TODO: double check

	gsi_channel_props.max_prefetch = GSI_ONE_PREFETCH_SEG; // TODO: double check
	gsi_channel_props.low_weight = 1; // TODO: double check

#if 0
	gsi_channel_props.prefetch_mode = ; // set by ipa eth
	gsi_channel_props.empty_lvl_threshold = ;  // set by ipa eth
#endif

	gsi_channel_props.xfer_cb = NULL; // FIXME: do we need this?
	gsi_channel_props.err_cb = NULL; // FIXME: have a handler to log error
	gsi_channel_props.chan_user_data = ch; // FIXME: do we need this?

	memset(&ch_scratch, 0, sizeof(ch_scratch));

	rc = ipa_eth_gsi_iommu_pamap(ch->buff_mem.daddr, ch->buff_mem.paddr,
				     ch->buff_mem.size,
				     IOMMU_READ | IOMMU_WRITE, false);
	if (rc) {
		pr_crit("AQC: FAILED TO MAP TX BUFFER MEM");
		return rc;
	}

	pr_crit("AQC: Tx: buff: paddr = %p, daddr = %p", ch->buff_mem.paddr, ch->buff_mem.daddr);

	ch_scratch.data.word2 |= (ilog2(2048) << 16);

	pr_crit("AQC: %s: ch_scratch.data.word1=%x", __func__, ch_scratch.data.word1);
	pr_crit("AQC: %s: ch_scratch.data.word2=%x", __func__, ch_scratch.data.word2);

	rc = ipa_eth_gsi_alloc(ch, &gsi_evt_ring_props, NULL, NULL,
			&gsi_channel_props, &ch_scratch,
			&aqo_dev->ch_tx.gsi_db.paddr);
	if (rc) {
		pr_crit("AQC: failed to alloc gsi");
		return rc;
	}

	aqo_dev->ch_tx.gsi_ch = gsi_channel_props.ch_id;

	rc = ipa_eth_gsi_ring_evtring(ch,
		gsi_evt_ring_props.ring_base_addr + gsi_evt_ring_props.ring_len);
	if (rc) {
		pr_crit("AQC: failed to ring evt ring db");
		goto err_ring_ev;
	}

	return 0;

err_ring_ev:
	ipa_eth_gsi_dealloc(ch);

	return rc;
}

int aqo_gsi_deinit_tx(struct aqo_device *aqo_dev)
{
	struct ipa_eth_channel *ch = AQO_ETHDEV(aqo_dev)->ch_tx;

	// TODO: check return value
	ipa_eth_gsi_dealloc(ch);

	// FIXME: Unmap addresses from SMMU

	return 0;
}

int aqo_gsi_start_tx(struct aqo_device *aqo_dev)
{
	int rc;
	struct ipa_eth_channel *ch = AQO_ETHDEV(aqo_dev)->ch_tx;
	u32 chdb_val = AQO_ETHDEV(aqo_dev)->ch_tx->desc_mem.size / 16;

	rc = ipa_eth_gsi_start(ch);
	if (rc) {
		pr_crit("AQC: failed to start gsi channel");
		return rc;
	}

	rc = ipa_eth_gsi_ring_channel(ch, chdb_val);
	if (rc) {
		pr_crit("AQC: failed to ring ch db");
		ipa_eth_gsi_stop(ch);
		return rc;
	}

	return 0;
}

int aqo_gsi_stop_tx(struct aqo_device *aqo_dev)
{
	return ipa_eth_gsi_stop(AQO_ETHDEV(aqo_dev)->ch_tx);
}
