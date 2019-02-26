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

#include "aqo_i.h"

static int aqo_netdev_process_skb(struct ipa_eth_channel *ch,
	struct sk_buff *skb)
{
	struct aqo_device *aqo_dev = (struct aqo_device *) ch->ipa_priv;

	if (IPA_ETH_CH_IS_RX(ch)) {
		return AQO_NETOPS(aqo_dev)->receive_skb(AQO_ETHDEV(aqo_dev), skb);
	}

	// TODO: Log

	if (IPA_ETH_CH_IS_TX(ch)) {
		// TODO: do we need this?
		return AQO_NETOPS(aqo_dev)->transmit_skb(AQO_ETHDEV(aqo_dev), skb);
	}

	// FIXME: Log

	return -ENODEV;
}

int aqo_netdev_init_rx_channel(struct aqo_device *aqo_dev)
{
	struct ipa_eth_channel *ch = NULL;

	ch = AQO_NETOPS(aqo_dev)->request_channel(
			AQO_ETHDEV(aqo_dev), IPA_ETH_DIR_RX, 0, 0);
	if (IS_ERR_OR_NULL(ch)) {
		return ch ? PTR_ERR(ch) : -ENOMEM;
	}

	pr_crit("AQC: aqo_netdev_init_rx_channel: queue=%d", ch->queue);

	ch->ipa_priv = aqo_dev;

	ch->ipa_ep_num = -1; // IPA_EP_NOT_ALLOCATED
	ch->ipa_client = IPA_CLIENT_AQC_ETHERNET_PROD;
	ch->process_skb = aqo_netdev_process_skb;

	AQO_ETHDEV(aqo_dev)->ch_rx = ch;

	return 0;
}

int aqo_netdev_deinit_rx_channel(struct aqo_device *aqo_dev)
{
	AQO_NETOPS(aqo_dev)->release_channel(AQO_ETHDEV(aqo_dev)->ch_rx);
	AQO_ETHDEV(aqo_dev)->ch_rx = NULL;
	return 0;
}

int aqo_netdev_init_rx_event(struct aqo_device *aqo_dev)
{
	u64 msi_addr;
	u32 msi_data;

	if (aqo_dev->ch_rx.proxy.agent == AQO_PROXY_UC) {
		msi_addr = aqo_dev->ch_rx.proxy.uc_ctx.msi_addr.paddr;
		msi_data = aqo_dev->ch_rx.proxy.uc_ctx.msi_data;
	} else if (aqo_dev->ch_rx.proxy.agent == AQO_PROXY_HOST) {
		msi_addr = aqo_dev->ch_rx.proxy.host_ctx.msi_addr.paddr;
		msi_data = aqo_dev->ch_rx.proxy.host_ctx.msi_data;
	} else {
		pr_crit("AQC: unknown rx proxy agent");
		return -EINVAL;
	}

	pr_crit("AQC: aqo_netdev_init_rx_event: %llx %lx", msi_addr, msi_data);

	return AQO_NETOPS(aqo_dev)->request_event(
			AQO_ETHDEV(aqo_dev)->ch_rx, IPA_ETH_DEV_EV_RX_INT,
			msi_addr, msi_data);
}

int aqo_netdev_deinit_rx_event(struct aqo_device *aqo_dev)
{
	AQO_NETOPS(aqo_dev)->release_event(
		AQO_ETHDEV(aqo_dev)->ch_rx, IPA_ETH_DEV_EV_RX_INT);

	return 0;
}

int aqo_netdev_start_rx(struct aqo_device *aqo_dev)
{
	int rc_mod, rc_ev, rc_ch;
	struct ipa_eth_channel *ch = AQO_ETHDEV(aqo_dev)->ch_rx;

	rc_mod = AQO_NETOPS(aqo_dev)->moderate_event(
			ch, IPA_ETH_DEV_EV_RX_INT, 0, 0,
			aqo_dev->rx_mod_usecs, aqo_dev->rx_mod_usecs * 2);

	rc_ev = AQO_NETOPS(aqo_dev)->enable_event(ch, IPA_ETH_DEV_EV_RX_INT);


	rc_ch = AQO_NETOPS(aqo_dev)->enable_channel(ch);

	return rc_mod || rc_ev || rc_ch;
}

int aqo_netdev_stop_rx(struct aqo_device *aqo_dev)
{
	int rc_ev, rc_ch;
	struct ipa_eth_channel *ch = AQO_ETHDEV(aqo_dev)->ch_rx;

	rc_ch = AQO_NETOPS(aqo_dev)->disable_channel(ch);
	rc_ev = AQO_NETOPS(aqo_dev)->disable_event(ch, IPA_ETH_DEV_EV_RX_INT);

	return rc_ev || rc_ch;
}

int aqo_netdev_init_tx_channel(struct aqo_device *aqo_dev)
{
	struct ipa_eth_channel *ch = NULL;

	ch = AQO_NETOPS(aqo_dev)->request_channel(AQO_ETHDEV(aqo_dev),
			IPA_ETH_DIR_TX, 0, 0);
	if (IS_ERR_OR_NULL(ch)) {
		return ch ? PTR_ERR(ch) : -ENOMEM;
	}

	pr_crit("AQC: aqc_request_tx_channel: queue=%d", ch->queue);

	ch->ipa_priv = aqo_dev;

	ch->ipa_ep_num = -1; /* IPA_EP_NOT_ALLOCATED */
	ch->ipa_client = IPA_CLIENT_AQC_ETHERNET_CONS;
	ch->process_skb = aqo_netdev_process_skb;

	AQO_ETHDEV(aqo_dev)->ch_tx = ch;

	return 0;
}

int aqo_netdev_deinit_tx_channel(struct aqo_device *aqo_dev)
{
	AQO_NETOPS(aqo_dev)->release_channel(AQO_ETHDEV(aqo_dev)->ch_tx);
	AQO_ETHDEV(aqo_dev)->ch_tx = NULL;
	return 0;
}

int aqo_netdev_init_tx_event(struct aqo_device *aqo_dev)
{
	return AQO_NETOPS(aqo_dev)->request_event(
			AQO_ETHDEV(aqo_dev)->ch_tx, IPA_ETH_DEV_EV_TX_PTR,
			aqo_dev->ch_tx.gsi_db.paddr, 0);
}

int aqo_netdev_deinit_tx_event(struct aqo_device *aqo_dev)
{
	AQO_NETOPS(aqo_dev)->release_event(
		AQO_ETHDEV(aqo_dev)->ch_tx, IPA_ETH_DEV_EV_TX_PTR);

	return 0;
}

int aqo_netdev_start_tx(struct aqo_device *aqo_dev)
{
	int rc_ev, rc_ch;
	struct ipa_eth_channel *ch = AQO_ETHDEV(aqo_dev)->ch_tx;

	// Note: Order matters - event need to be enabled before channel for
	//       tx head ptr wrb. Need to confirm why so ?
	rc_ev = AQO_NETOPS(aqo_dev)->enable_event(ch, IPA_ETH_DEV_EV_TX_PTR);
	rc_ch = AQO_NETOPS(aqo_dev)->enable_channel(ch);

	return rc_ev || rc_ch;
}

int aqo_netdev_stop_tx(struct aqo_device *aqo_dev)
{
	int rc_ev, rc_ch;
	struct ipa_eth_channel *ch = AQO_ETHDEV(aqo_dev)->ch_tx;

	// Note: Order matters - event need to be enabled before channel for
	//       tx head ptr wrb. Need to confirm why so ?
	rc_ch = AQO_NETOPS(aqo_dev)->disable_channel(ch);
	rc_ev = AQO_NETOPS(aqo_dev)->disable_event(ch, IPA_ETH_DEV_EV_TX_PTR);

	return rc_ev || rc_ch;
}
