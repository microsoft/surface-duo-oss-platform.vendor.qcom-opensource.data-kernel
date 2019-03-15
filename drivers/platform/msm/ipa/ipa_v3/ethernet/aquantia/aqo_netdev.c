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
	struct aqo_device *aqo_dev = (struct aqo_device *) ch->od_priv;

	if (IPA_ETH_CH_IS_RX(ch))
		return AQO_NETOPS(aqo_dev)->receive_skb(AQO_ETHDEV(aqo_dev), skb);

	if (IPA_ETH_CH_IS_TX(ch))
		return AQO_NETOPS(aqo_dev)->transmit_skb(AQO_ETHDEV(aqo_dev), skb);

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

	aqo_log(aqo_dev, "Allocated AQC Rx channel %u", ch->queue);

	ch->od_priv = aqo_dev;

	ch->ipa_ep_num = -1; /* IPA_EP_NOT_ALLOCATED */
	ch->ipa_client = IPA_CLIENT_AQC_ETHERNET_PROD;
	ch->process_skb = aqo_netdev_process_skb;

	AQO_ETHDEV(aqo_dev)->ch_rx = ch;

	return 0;
}

int aqo_netdev_deinit_rx_channel(struct aqo_device *aqo_dev)
{
	AQO_NETOPS(aqo_dev)->release_channel(AQO_ETHDEV(aqo_dev)->ch_rx);
	AQO_ETHDEV(aqo_dev)->ch_rx = NULL;

	aqo_log(aqo_dev, "Deallocated AQC Rx channel");

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
		aqo_log_bug(aqo_dev, "Invalid MSI proxy agent %u",
				aqo_dev->ch_rx.proxy.agent);
		return -EINVAL;
	}

	aqo_log(aqo_dev, "Requesting AQC Rx MSI to address %llx using data %lx",
			msi_addr, msi_data);

	return AQO_NETOPS(aqo_dev)->request_event(
			AQO_ETHDEV(aqo_dev)->ch_rx, IPA_ETH_DEV_EV_RX_INT,
			msi_addr, msi_data);
}

int aqo_netdev_deinit_rx_event(struct aqo_device *aqo_dev)
{
	aqo_log(aqo_dev, "Releasing AQC Rx MSI");

	AQO_NETOPS(aqo_dev)->release_event(
		AQO_ETHDEV(aqo_dev)->ch_rx, IPA_ETH_DEV_EV_RX_INT);

	return 0;
}

int aqo_netdev_start_rx(struct aqo_device *aqo_dev)
{
	int rc;
	struct ipa_eth_channel *ch = AQO_ETHDEV(aqo_dev)->ch_rx;

	rc = AQO_NETOPS(aqo_dev)->moderate_event(
			ch, IPA_ETH_DEV_EV_RX_INT, 0, 0,
			aqo_dev->rx_mod_usecs, aqo_dev->rx_mod_usecs * 2);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to set AQC Rx MSI moderation");
		return rc;
	}

	aqo_log(aqo_dev, "AQC Rx MSI moderation set to [%u,%u] usecs",
			aqo_dev->rx_mod_usecs, aqo_dev->rx_mod_usecs * 2);

	rc = AQO_NETOPS(aqo_dev)->enable_event(ch, IPA_ETH_DEV_EV_RX_INT);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to enable AQC Rx MSI event");
		return rc;
	}

	aqo_log(aqo_dev, "Enabled AQC Rx MSI event");

	rc = AQO_NETOPS(aqo_dev)->enable_channel(ch);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to enable AQC Rx channel");
		AQO_NETOPS(aqo_dev)->disable_event(ch, IPA_ETH_DEV_EV_RX_INT);
		return rc;
	}

	aqo_log(aqo_dev, "Enabled AQC Rx channel");
	aqo_log(aqo_dev, "Started AQC Rx");

	return 0;
}

int aqo_netdev_stop_rx(struct aqo_device *aqo_dev)
{
	int rc_ev, rc_ch;
	struct ipa_eth_channel *ch = AQO_ETHDEV(aqo_dev)->ch_rx;

	rc_ch = AQO_NETOPS(aqo_dev)->disable_channel(ch);
	if (rc_ch)
		aqo_log_err(aqo_dev, "Failed to stop AQC Rx channel");
	else
		aqo_log(aqo_dev, "Stopped AQC Rx channel");

	rc_ev = AQO_NETOPS(aqo_dev)->disable_event(ch, IPA_ETH_DEV_EV_RX_INT);
	if (rc_ch)
		aqo_log_err(aqo_dev, "Failed to disable AQC Rx MSI event");
	else
		aqo_log(aqo_dev, "Disabled AQC Rx MSI event");

	if (rc_ev || rc_ch)
		return -EFAULT;

	aqo_log(aqo_dev, "Stopped AQC Rx");

	return 0;
}

int aqo_netdev_init_tx_channel(struct aqo_device *aqo_dev)
{
	struct ipa_eth_channel *ch = NULL;

	ch = AQO_NETOPS(aqo_dev)->request_channel(AQO_ETHDEV(aqo_dev),
			IPA_ETH_DIR_TX, 0, 0);
	if (IS_ERR_OR_NULL(ch)) {
		return ch ? PTR_ERR(ch) : -ENOMEM;
	}

	aqo_log(aqo_dev, "Allocated AQC Tx channel %u", ch->queue);

	ch->od_priv = aqo_dev;

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

	aqo_log(aqo_dev, "Deallocated AQC Tx channel");

	return 0;
}

int aqo_netdev_init_tx_event(struct aqo_device *aqo_dev)
{
	aqo_log(aqo_dev, "Requesting AQC Tx head pointer write-back");

	return AQO_NETOPS(aqo_dev)->request_event(
			AQO_ETHDEV(aqo_dev)->ch_tx, IPA_ETH_DEV_EV_TX_PTR,
			aqo_dev->ch_tx.gsi_db.paddr, 0);
}

int aqo_netdev_deinit_tx_event(struct aqo_device *aqo_dev)
{
	aqo_log(aqo_dev, "Releasing AQC Tx head pointer write-back");

	AQO_NETOPS(aqo_dev)->release_event(
		AQO_ETHDEV(aqo_dev)->ch_tx, IPA_ETH_DEV_EV_TX_PTR);

	return 0;
}

int aqo_netdev_start_tx(struct aqo_device *aqo_dev)
{
	int rc;
	struct ipa_eth_channel *ch = AQO_ETHDEV(aqo_dev)->ch_tx;

	/* Head pointer write-back need to be enabled before enabling channel
	 * in order to avoid missing events
	 */
	rc = AQO_NETOPS(aqo_dev)->enable_event(ch, IPA_ETH_DEV_EV_TX_PTR);
	if (rc) {
		aqo_log_err(aqo_dev,
			"Failed to enable AQC Tx Head pointer write-back");
		return rc;
	}

	aqo_log(aqo_dev, "Enabled AQC Tx Head pointer write-back");

	rc = AQO_NETOPS(aqo_dev)->enable_channel(ch);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to enable AQC Tx channel");
		AQO_NETOPS(aqo_dev)->disable_event(ch, IPA_ETH_DEV_EV_TX_PTR);
		return rc;
	}

	aqo_log(aqo_dev, "Enabled AQC Tx channel");

	return 0;
}

int aqo_netdev_stop_tx(struct aqo_device *aqo_dev)
{
	int rc_ev, rc_ch;
	struct ipa_eth_channel *ch = AQO_ETHDEV(aqo_dev)->ch_tx;

	rc_ch = AQO_NETOPS(aqo_dev)->disable_channel(ch);
	if (rc_ch)
		aqo_log_err(aqo_dev, "Failed to stop AQC Tx channel");
	else
		aqo_log(aqo_dev, "Stopped AQC Tx channel");

	rc_ev = AQO_NETOPS(aqo_dev)->disable_event(ch, IPA_ETH_DEV_EV_TX_PTR);
	if (rc_ch)
		aqo_log_err(aqo_dev,
			"Failed to disable AQC Tx head pointer write-back");
	else
		aqo_log(aqo_dev, "Disabled AQC Tx head pointer write-back");

	if (rc_ev || rc_ch) {
		aqo_log_err(aqo_dev, "Failed to fully stop AQC Tx");
		return -EFAULT;
	}

	aqo_log(aqo_dev, "Stopped AQC Rx");

	return 0;
}
