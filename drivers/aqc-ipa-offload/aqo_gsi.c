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

#include "aqo_i.h"

#define AQO_GSI_CH_LOW_WEIGHT 1

#define ASSERT_SSR_MUTEX(ad) do { \
	if (!mutex_is_locked(&ad->ssr_mutex)) \
		aqo_log_bug(ad, "API called without holding SSR mutex"); \
} while (0)

static const struct gsi_chan_props aqo_gsi_rx_ch_fixed_props = {
	.prot = GSI_CHAN_PROT_AQC,
	.dir = GSI_CHAN_DIR_TO_GSI,
	.re_size = GSI_CHAN_RE_SIZE_16B,
	.max_prefetch = GSI_ONE_PREFETCH_SEG,
	.low_weight = AQO_GSI_CH_LOW_WEIGHT,
};

static const struct gsi_evt_ring_props aqo_gsi_rx_evt_ring_fixed_props = {
	.intf = GSI_EVT_CHTYPE_AQC_EV,
	.intr = GSI_INTR_MSI,
	.re_size = GSI_EVT_RING_RE_SIZE_16B,
	.exclusive = true,
};

static const struct gsi_chan_props aqo_gsi_tx_ch_fixed_props = {
	.prot = GSI_CHAN_PROT_AQC,
	.dir = GSI_CHAN_DIR_FROM_GSI,
	.re_size = GSI_CHAN_RE_SIZE_16B,
	.max_prefetch = GSI_ONE_PREFETCH_SEG,
	.low_weight = AQO_GSI_CH_LOW_WEIGHT,
	.use_db_eng = GSI_CHAN_DB_MODE,
};

static const struct gsi_evt_ring_props aqo_gsi_tx_evt_ring_fixed_props = {
	.intf = GSI_EVT_CHTYPE_AQC_EV,
	.intr = GSI_INTR_MSI,
	.re_size = GSI_EVT_RING_RE_SIZE_16B,
	.exclusive = true,
};

static struct ipa_eth_resource *get_desc_cb_mem(struct aqo_channel *aqo_ch)
{
	struct ipa_eth_channel *ch = aqo_ch->eth_ch;
	struct ipa_eth_channel_mem *desc =
			list_first_entry(&ch->desc_mem,
				struct ipa_eth_channel_mem, mem_list_entry);

	return ipa_eth_net_ch_to_cb_mem(ch, desc, IPA_ETH_HW_GSI);
}

static struct ipa_eth_resource *get_buff_cb_mem(struct aqo_channel *aqo_ch)
{
	struct ipa_eth_channel *ch = aqo_ch->eth_ch;
	struct ipa_eth_channel_mem *buff = list_first_entry(
		&ch->buff_mem, struct ipa_eth_channel_mem, mem_list_entry);

	return ipa_eth_net_ch_to_cb_mem(ch, buff, IPA_ETH_HW_IPA);
}

static int aqo_gsi_init_ch_config_common(
		struct aqo_channel *aqo_ch,
		const struct gsi_chan_props *fixed_props)
{
	struct gsi_chan_props *props = &aqo_ch->gsi_config.ch_props;
	struct ipa_eth_resource *desc_mem = get_desc_cb_mem(aqo_ch);

	if (!desc_mem) {
		aqo_log_err(NULL, "Failed to get desc cb mem information");
		return -EFAULT;
	}

	if (fixed_props)
		memcpy(props, fixed_props, sizeof(*props));

	props->ring_len = desc_mem->size;
	props->ring_base_addr = desc_mem->daddr;

	return 0;
}

static int aqo_gsi_init_evt_ring_config_common(
		struct aqo_channel *aqo_ch,
		const struct gsi_evt_ring_props *fixed_props)
{
	struct gsi_evt_ring_props *props = &aqo_ch->gsi_config.evt_ring_props;
	const struct ipa_eth_resource *desc_mem = get_desc_cb_mem(aqo_ch);

	if (!desc_mem) {
		aqo_log_err(NULL, "Failed to get desc cb mem information");
		return -EFAULT;
	}

	if (fixed_props)
		memcpy(props, fixed_props, sizeof(*props));

	props->ring_len = desc_mem->size;
	props->ring_base_addr = desc_mem->daddr;

	/* We ring both Rx and Tx evt ring DBs with same value */
	aqo_ch->gsi_config.evt_ring_db_val = desc_mem->daddr + desc_mem->size;

	return 0;
}

static int aqo_gsi_init_rx_ch_props(struct aqo_channel *aqo_ch)
{
	int rc;
	enum aqo_proxy_mode proxy_mode = aqo_ch->proxy.mode;
	struct gsi_chan_props *props = &aqo_ch->gsi_config.ch_props;

	rc = aqo_gsi_init_ch_config_common(aqo_ch, &aqo_gsi_rx_ch_fixed_props);
	if (rc) {
		aqo_log_err(NULL, "Failed to init Rx channel common props");
		return rc;
	}

	props->use_db_eng = (proxy_mode == AQO_PROXY_MODE_HEADPTR) ?
					GSI_CHAN_DIRECT_MODE : GSI_CHAN_DB_MODE;

	return 0;
}

static int aqo_gsi_init_rx_ch_scratch(struct aqo_channel *aqo_ch, u64 head_ptr)
{
	const u32 LOWER_8BITS =  (BIT(8) - 1);
	const u32 LOWER_9BITS =  (BIT(9) - 1);

	struct aqo_gsi_rx_ch_scratch *rx_scratch =
					&aqo_ch->gsi_config.ch_scratch.rx;

	size_t buff_size = aqo_ch->eth_ch->mem_params.buff.size;
	struct ipa_eth_resource *buff_mem = get_buff_cb_mem(aqo_ch);

	if (!buff_mem) {
		aqo_log_err(NULL, "Failed to get buff cb mem information");
		return -EFAULT;
	}

	if (upper_32_bits(buff_mem->daddr) & ~LOWER_8BITS)
		aqo_log_bug(NULL,
			"Excess Rx buffer memory address bits are ignored");

	if (upper_32_bits(head_ptr) & ~LOWER_9BITS)
		aqo_log_bug(NULL,
			"Excess Head Pointer address bits are ignored");

	rx_scratch->buff_mem_lsb = lower_32_bits(buff_mem->daddr);
	rx_scratch->buff_msm_msb = upper_32_bits(buff_mem->daddr) & LOWER_8BITS;

	rx_scratch->buff_size_log2 = ilog2(buff_size);

	rx_scratch->head_ptr_lsb = lower_32_bits(head_ptr);
	rx_scratch->head_ptr_msb = upper_32_bits(head_ptr) & LOWER_9BITS;

	return 0;
}

static int aqo_gsi_init_rx_evt_ring_props(
		struct aqo_channel *aqo_ch, u64 tail_ptr)
{
	int rc;
	struct gsi_evt_ring_props *props = &aqo_ch->gsi_config.evt_ring_props;

	rc = aqo_gsi_init_evt_ring_config_common(aqo_ch,
				&aqo_gsi_rx_evt_ring_fixed_props);
	if (rc) {
		aqo_log_err(NULL, "Failed to init Rx event ring comon props");
		return rc;
	}

	props->msi_addr = tail_ptr;

	props->int_modt = aqo_ch->gsi_modt;
	props->int_modc = aqo_ch->gsi_modc;

	return 0;
}

static int aqo_gsi_init_rx_config(struct aqo_device *aqo_dev)
{
	int rc;
	struct aqo_channel *aqo_ch = &aqo_dev->ch_rx;
	struct ipa_eth_channel *ch = aqo_ch->eth_ch;
	u64 tail_ptr = AQC_RX_TAIL_PTR(aqo_dev->regs_base.paddr, ch->queue);
	u64 head_ptr = AQC_RX_HEAD_PTR(aqo_dev->regs_base.paddr, ch->queue);

	if (aqo_dev->pci_direct) {
		tail_ptr = AQO_PCI_DIRECT_SET(tail_ptr);
		head_ptr = AQO_PCI_DIRECT_SET(head_ptr);
	} else {
		aqo_log_bug(aqo_dev, "Only PCI direct access is supported");
		return -EFAULT;
	}

	memset(&aqo_ch->gsi_config, 0, sizeof(aqo_ch->gsi_config));

	rc = aqo_gsi_init_rx_ch_props(aqo_ch);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to init Rx GSI channel props");
		return rc;
	}

	rc = aqo_gsi_init_rx_ch_scratch(aqo_ch, head_ptr);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to init Rx GSI channel scratch");
		return rc;
	}

	rc = aqo_gsi_init_rx_evt_ring_props(aqo_ch, tail_ptr);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to init Rx GSI event ring props");
		return rc;
	}

	return 0;
}

static int __aqo_gsi_init_rx(struct aqo_device *aqo_dev)
{
	int rc;
	struct aqo_channel *aqo_ch = &aqo_dev->ch_rx;
	struct aqo_gsi_config *gsi = &aqo_ch->gsi_config;
	struct ipa_eth_channel *ch = aqo_ch->eth_ch;

	ASSERT_SSR_MUTEX(aqo_dev);

	rc = aqo_gsi_init_rx_config(aqo_dev);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to init Rx GSI config");
		return rc;
	}

	rc = ipa_eth_gsi_alloc(ch,
			&gsi->evt_ring_props,
			&gsi->evt_ring_scratch.scratch,
			&gsi->evt_ring_db_addr,
			&gsi->ch_props,
			&gsi->ch_scratch.scratch,
			&gsi->ch_db_addr);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to alloc Rx GSI rings");
		goto err_gsi_alloc;
	}

	aqo_ch->gsi_ch = gsi->ch_props.ch_id;

	/* GSI channel doorbell address is needed for A7 proxy */
	aqo_ch->gsi_db.paddr = gsi->ch_db_addr;

	rc = ipa_eth_gsi_ring_evtring(ch, gsi->evt_ring_db_val);
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

int aqo_gsi_init_rx(struct aqo_device *aqo_dev)
{
	int rc = 0;

	mutex_lock(&aqo_dev->ssr_mutex);

	if (test_bit(AQO_DEV_S_IN_SSR, &aqo_dev->state))
		aqo_log(aqo_dev, "Rx GSI will init after SSR");
	else
		rc = __aqo_gsi_init_rx(aqo_dev);

	if (!rc)
		set_bit(AQO_DEV_S_RX_GSI_INIT, &aqo_dev->state);

	mutex_unlock(&aqo_dev->ssr_mutex);

	return rc;
}

static int __aqo_gsi_deinit_rx(struct aqo_device *aqo_dev)
{
	int rc;
	struct ipa_eth_channel *ch = aqo_dev->ch_rx.eth_ch;

	ASSERT_SSR_MUTEX(aqo_dev);

	rc = ipa_eth_gsi_dealloc(ch);
	if (rc)
		aqo_log_err(aqo_dev, "Failed to deinit Rx GSI rings");
	else
		aqo_log(aqo_dev, "Deinited Rx GSI rings");

	return rc;
}

int aqo_gsi_deinit_rx(struct aqo_device *aqo_dev)
{
	int rc = 0;

	mutex_lock(&aqo_dev->ssr_mutex);

	if (test_bit(AQO_DEV_S_IN_SSR, &aqo_dev->state))
		aqo_log(aqo_dev, "Rx GSI will remain deinited after SSR");
	else
		rc = __aqo_gsi_deinit_rx(aqo_dev);

	if (!rc)
		clear_bit(AQO_DEV_S_RX_GSI_INIT, &aqo_dev->state);

	mutex_unlock(&aqo_dev->ssr_mutex);

	return rc;

}

static int __aqo_gsi_start_rx(struct aqo_device *aqo_dev)
{
	int rc;
	struct ipa_eth_channel *ch = aqo_dev->ch_rx.eth_ch;

	ASSERT_SSR_MUTEX(aqo_dev);

	rc = ipa_eth_gsi_start(ch);
	if (rc)
		aqo_log_err(aqo_dev, "Failed to start Rx GSI rings");
	else
		aqo_log(aqo_dev, "Started Rx GSI rings");

	return rc;
}

int aqo_gsi_start_rx(struct aqo_device *aqo_dev)
{
	int rc = 0;

	mutex_lock(&aqo_dev->ssr_mutex);

	if (test_bit(AQO_DEV_S_IN_SSR, &aqo_dev->state))
		aqo_log(aqo_dev, "Rx GSI will start after SSR");
	else
		rc = __aqo_gsi_start_rx(aqo_dev);

	if (!rc)
		set_bit(AQO_DEV_S_RX_GSI_START, &aqo_dev->state);

	mutex_unlock(&aqo_dev->ssr_mutex);

	return rc;
}

static int __aqo_gsi_stop_rx(struct aqo_device *aqo_dev)
{
	int rc;
	struct ipa_eth_channel *ch = aqo_dev->ch_rx.eth_ch;

	ASSERT_SSR_MUTEX(aqo_dev);

	rc = ipa_eth_gsi_stop(ch);
	if (rc)
		aqo_log_err(aqo_dev, "Failed to stop Rx GSI rings");
	else
		aqo_log(aqo_dev, "Stopped Rx GSI rings");

	return rc;
}

int aqo_gsi_stop_rx(struct aqo_device *aqo_dev)
{
	int rc = 0;

	mutex_lock(&aqo_dev->ssr_mutex);

	if (test_bit(AQO_DEV_S_IN_SSR, &aqo_dev->state))
		aqo_log(aqo_dev, "Rx GSI will remain stopped after SSR");
	else
		rc = __aqo_gsi_stop_rx(aqo_dev);

	if (!rc)
		clear_bit(AQO_DEV_S_RX_GSI_START, &aqo_dev->state);

	mutex_unlock(&aqo_dev->ssr_mutex);

	return rc;
}

static int aqo_gsi_init_tx_ch_props(struct aqo_channel *aqo_ch)
{
	int rc;

	rc = aqo_gsi_init_ch_config_common(aqo_ch, &aqo_gsi_tx_ch_fixed_props);
	if (rc)
		aqo_log_err(NULL, "Failed to init Tx channel common props");

	return rc;
}

static int aqo_gsi_init_tx_ch_scratch(struct aqo_channel *aqo_ch)
{
	struct aqo_gsi_tx_ch_scratch *tx_scratch =
					&aqo_ch->gsi_config.ch_scratch.tx;

	size_t buff_size = aqo_ch->eth_ch->mem_params.buff.size;

	tx_scratch->buff_size_log2 = ilog2(buff_size);

	return 0;
}

static int aqo_gsi_init_tx_evt_ring_props(
		struct aqo_channel *aqo_ch, u64 tail_ptr)
{
	int rc;
	struct gsi_evt_ring_props *props = &aqo_ch->gsi_config.evt_ring_props;

	rc = aqo_gsi_init_evt_ring_config_common(aqo_ch,
				&aqo_gsi_tx_evt_ring_fixed_props);
	if (rc) {
		aqo_log_err(NULL, "Failed to init Tx event ring comon props");
		return rc;
	}

	props->msi_addr = tail_ptr;

	props->int_modt = aqo_ch->gsi_modt;
	props->int_modc = aqo_ch->gsi_modc;

	return 0;
}

static int aqo_gsi_init_tx_evt_ring_scratch(
		struct aqo_channel *aqo_ch, u32 wrb_mod)
{
	struct aqo_gsi_tx_evt_ring_scratch *tx_scratch =
					&aqo_ch->gsi_config.evt_ring_scratch.tx;

	tx_scratch->head_ptr_wrb_thresh = wrb_mod;

	return 0;
}

static int aqo_gsi_init_tx_config(struct aqo_device *aqo_dev)
{
	int rc;
	struct aqo_channel *aqo_ch = &aqo_dev->ch_tx;
	struct ipa_eth_channel *ch = aqo_ch->eth_ch;
	u64 tail_ptr = AQC_TX_TAIL_PTR(aqo_dev->regs_base.paddr, ch->queue);
	u32 wrb_mod = aqo_dev->tx_wrb_mod_count;

	if (aqo_dev->pci_direct) {
		tail_ptr = AQO_PCI_DIRECT_SET(tail_ptr);
	} else {
		aqo_log_bug(aqo_dev, "Only PCI direct access is supported");
		return -EFAULT;
	}

	memset(&aqo_ch->gsi_config, 0, sizeof(aqo_ch->gsi_config));

	rc = aqo_gsi_init_tx_ch_props(aqo_ch);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to init Tx GSI channel props");
		return rc;
	}

	rc = aqo_gsi_init_tx_ch_scratch(aqo_ch);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to init Tx GSI channel scratch");
		return rc;
	}

	rc = aqo_gsi_init_tx_evt_ring_props(aqo_ch, tail_ptr);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to init Tx GSI event ring props");
		return rc;
	}

	rc = aqo_gsi_init_tx_evt_ring_scratch(aqo_ch, wrb_mod);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to init Tx GSI channel scratch");
		return rc;
	}

	/* Ring channel DB with the last descriptor in the Tx ring so that
	 * IPA will get all the buffers (except the last one) as credits.
	 */
	aqo_ch->gsi_config.ch_db_val = ch->mem_params.desc.count - 1;

	return 0;
}

static int __aqo_gsi_init_tx(struct aqo_device *aqo_dev)
{
	int rc;
	struct aqo_channel *aqo_ch = &aqo_dev->ch_tx;
	struct aqo_gsi_config *gsi = &aqo_ch->gsi_config;
	struct ipa_eth_channel *ch = aqo_ch->eth_ch;

	ASSERT_SSR_MUTEX(aqo_dev);

	rc = aqo_gsi_init_tx_config(aqo_dev);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to init Tx GSI config");
		return rc;
	}

	rc = ipa_eth_gsi_alloc(ch,
			&gsi->evt_ring_props,
			&gsi->evt_ring_scratch.scratch,
			&gsi->evt_ring_db_addr,
			&gsi->ch_props,
			&gsi->ch_scratch.scratch,
			&gsi->ch_db_addr);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to alloc Tx GSI rings");
		goto err_gsi_alloc;
	} else {
		aqo_log(aqo_dev, "Successfully allocated Tx GSI rings");
	}

	aqo_ch->gsi_ch = gsi->ch_props.ch_id;
	aqo_ch->gsi_db.paddr = gsi->ch_db_addr;

	rc = ipa_eth_gsi_ring_evtring(ch, gsi->evt_ring_db_val);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to ring Tx GSI event ring");
		goto err_ring_ev;
	} else {
		aqo_log(aqo_dev,
			"Rang Tx GSI evt ring with value 0x%llx",
			gsi->evt_ring_db_val);
	}

	return 0;

err_ring_ev:
	ipa_eth_gsi_dealloc(ch);
err_gsi_alloc:
	return rc;
}

int aqo_gsi_init_tx(struct aqo_device *aqo_dev)
{
	int rc = 0;

	mutex_lock(&aqo_dev->ssr_mutex);

	if (test_bit(AQO_DEV_S_IN_SSR, &aqo_dev->state))
		aqo_log(aqo_dev, "Tx GSI will init after SSR");
	else
		rc = __aqo_gsi_init_tx(aqo_dev);

	if (!rc)
		set_bit(AQO_DEV_S_TX_GSI_INIT, &aqo_dev->state);

	mutex_unlock(&aqo_dev->ssr_mutex);

	return rc;
}

static int __aqo_gsi_deinit_tx(struct aqo_device *aqo_dev)
{
	int rc;
	struct ipa_eth_channel *ch = aqo_dev->ch_tx.eth_ch;

	ASSERT_SSR_MUTEX(aqo_dev);

	rc = ipa_eth_gsi_dealloc(ch);
	if (rc)
		aqo_log_err(aqo_dev, "Failed to deinit Tx GSI rings");
	else
		aqo_log(aqo_dev, "Deinited Tx GSI rings");

	return rc;
}

int aqo_gsi_deinit_tx(struct aqo_device *aqo_dev)
{
	int rc = 0;

	mutex_lock(&aqo_dev->ssr_mutex);

	if (test_bit(AQO_DEV_S_IN_SSR, &aqo_dev->state))
		aqo_log(aqo_dev, "Tx GSI will remain deinited after SSR");
	else
		rc = __aqo_gsi_deinit_tx(aqo_dev);

	if (!rc)
		clear_bit(AQO_DEV_S_TX_GSI_INIT, &aqo_dev->state);

	mutex_unlock(&aqo_dev->ssr_mutex);

	return rc;

}

static int __aqo_gsi_start_tx(struct aqo_device *aqo_dev)
{
	int rc;
	u64 *ch_db_val = &aqo_dev->ch_tx.gsi_config.ch_db_val;
	struct ipa_eth_channel *ch = aqo_dev->ch_tx.eth_ch;

	ASSERT_SSR_MUTEX(aqo_dev);

	rc = ipa_eth_gsi_start(ch);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to start Tx GSI rings");
		return rc;
	}

	aqo_log(aqo_dev, "Started Tx GSI rings");

	/* Provide credits to IPA only once after a tx init */
	if (*ch_db_val) {
		rc = ipa_eth_gsi_ring_channel(ch, *ch_db_val);
		if (rc) {
			aqo_log_err(aqo_dev,
				"Failed to ring Tx GSI channel DB");
			ipa_eth_gsi_stop(ch);
		} else {
			aqo_log(aqo_dev,
				"Rang Tx GSI channel DB with value %llu",
				*ch_db_val);
			*ch_db_val = 0;
		}
	}

	if (!rc)
		aqo_log(aqo_dev, "Tx GSI started successfully");
	else
		aqo_log_err(aqo_dev, "Failed to start Tx GSI");

	return rc;
}

int aqo_gsi_start_tx(struct aqo_device *aqo_dev)
{
	int rc = 0;

	mutex_lock(&aqo_dev->ssr_mutex);

	if (test_bit(AQO_DEV_S_IN_SSR, &aqo_dev->state))
		aqo_log(aqo_dev, "Tx GSI will start after SSR");
	else
		rc = __aqo_gsi_start_tx(aqo_dev);

	if (!rc)
		set_bit(AQO_DEV_S_TX_GSI_START, &aqo_dev->state);

	mutex_unlock(&aqo_dev->ssr_mutex);

	return rc;
}

static int __aqo_gsi_stop_tx(struct aqo_device *aqo_dev)
{
	int rc;
	struct ipa_eth_channel *ch = aqo_dev->ch_tx.eth_ch;

	ASSERT_SSR_MUTEX(aqo_dev);

	rc = ipa_eth_gsi_stop(ch);
	if (rc)
		aqo_log_err(aqo_dev, "Failed to stop Tx GSI rings");
	else
		aqo_log(aqo_dev, "Stopped Tx GSI rings");

	return rc;
}

int aqo_gsi_stop_tx(struct aqo_device *aqo_dev)
{
	int rc = 0;

	mutex_lock(&aqo_dev->ssr_mutex);

	if (test_bit(AQO_DEV_S_IN_SSR, &aqo_dev->state))
		aqo_log(aqo_dev, "Tx GSI will remain stopped after SSR");
	else
		rc = __aqo_gsi_stop_tx(aqo_dev);

	if (!rc)
		clear_bit(AQO_DEV_S_TX_GSI_START, &aqo_dev->state);

	mutex_unlock(&aqo_dev->ssr_mutex);

	return rc;
}

/* To be called with ssr mutex held */
int __aqo_gsi_prepare_ssr(struct aqo_device *aqo_dev)
{
	int rc = 0;

	ASSERT_SSR_MUTEX(aqo_dev);

	if (test_bit(AQO_DEV_S_TX_GSI_START, &aqo_dev->state))
		rc |= __aqo_gsi_stop_tx(aqo_dev);

	if (test_bit(AQO_DEV_S_RX_GSI_START, &aqo_dev->state))
		rc |= __aqo_gsi_stop_rx(aqo_dev);

	if (test_bit(AQO_DEV_S_TX_GSI_INIT, &aqo_dev->state))
		rc |= __aqo_gsi_deinit_tx(aqo_dev);

	if (test_bit(AQO_DEV_S_RX_GSI_INIT, &aqo_dev->state))
		rc |= __aqo_gsi_deinit_rx(aqo_dev);

	if (!rc)
		aqo_log(aqo_dev,
			"GSI channels are deinited");
	else
		aqo_log_err(aqo_dev, "Failed to fully deinit GSI channels");

	return rc;
}

int aqo_gsi_prepare_ssr(struct aqo_device *aqo_dev)
{
	int rc = -EFAULT;

	aqo_log(aqo_dev, "Device in SSR, preparing to reset GSI");

	mutex_lock(&aqo_dev->ssr_mutex);

	if (!test_and_set_bit(AQO_DEV_S_IN_SSR, &aqo_dev->state))
		rc = __aqo_gsi_prepare_ssr(aqo_dev);
	else
		aqo_log_bug(aqo_dev,
			"SSR PREPARE event received in the middle of SSR");

	mutex_unlock(&aqo_dev->ssr_mutex);

	if (!rc)
		aqo_log(aqo_dev,
			"GSI reset is complete, waiting for device to resume");
	else
		aqo_log_err(aqo_dev, "Failed to reset GSI");

	return rc;
}

static int __aqo_gsi_complete_ssr(struct aqo_device *aqo_dev)
{
	int rc = 0;

	ASSERT_SSR_MUTEX(aqo_dev);

	if (test_bit(AQO_DEV_S_TX_GSI_INIT, &aqo_dev->state))
		rc |= __aqo_gsi_init_tx(aqo_dev);

	if (test_bit(AQO_DEV_S_RX_GSI_INIT, &aqo_dev->state))
		rc |= __aqo_gsi_init_rx(aqo_dev);

	if (test_bit(AQO_DEV_S_TX_GSI_START, &aqo_dev->state))
		rc |= __aqo_gsi_start_tx(aqo_dev);

	if (test_bit(AQO_DEV_S_RX_GSI_START, &aqo_dev->state))
		rc |= __aqo_gsi_start_rx(aqo_dev);

	if (!rc)
		aqo_log(aqo_dev,
			"GSI channels resumed successfully");
	else
		aqo_log_err(aqo_dev, "Failed to resume GSI channels");

	return rc;
}

int aqo_gsi_complete_ssr(struct aqo_device *aqo_dev)
{
	int rc = -EFAULT;

	aqo_log(aqo_dev, "Device is ready, preparing to resume GSI");

	mutex_lock(&aqo_dev->ssr_mutex);

	/* Clear SSR flag before unlocking mutex so that a resuming thread
	 * would start with the updated state.
	 */
	if (test_and_clear_bit(AQO_DEV_S_IN_SSR, &aqo_dev->state))
		rc = __aqo_gsi_complete_ssr(aqo_dev);
	else
		aqo_log_bug(aqo_dev,
			"SSR COMPLETE event received when not in SSR");

	mutex_unlock(&aqo_dev->ssr_mutex);

	if (!rc)
		aqo_log(aqo_dev,
			"GSI resume is complete");
	else
		aqo_log_err(aqo_dev, "Failed to resume GSI");

	return rc;

}
