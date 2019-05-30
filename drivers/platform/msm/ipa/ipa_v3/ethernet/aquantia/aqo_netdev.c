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

#include <linux/iommu.h>

#include "aqo_i.h"

int aqo_netdev_init_rx_channel(struct aqo_device *aqo_dev)
{
	struct ipa_eth_channel_mem_params mem_params = {
		.desc = {
			.count = aqo_dev->ch_rx.ring_size,
			.hw_map_params = {
				[IPA_ETH_HW_GSI] = {
					.map = true,
					.sym = true,
					.read = true,
					.write = true,
				},
			},
		},
		.buff = {
			.size = aqo_dev->ch_rx.buff_size,
			.hw_map_params = {
				[IPA_ETH_HW_IPA] = {
					.map = true,
					.sym = true,
					.read = true,
					.write = false,
				},
			},
		},
	};
	struct ipa_eth_channel *ch = NULL;

	ch = ipa_eth_net_request_channel(AQO_ETHDEV(aqo_dev),
		IPA_CLIENT_AQC_ETHERNET_PROD, 0, 0, &mem_params);
	if (IS_ERR_OR_NULL(ch)) {
		return ch ? PTR_ERR(ch) : -ENOMEM;
	}

	aqo_log(aqo_dev, "Allocated AQC Rx channel %u", ch->queue);

	ch->od_priv = aqo_dev;
	aqo_dev->ch_rx.eth_ch = ch;

	return 0;
}

int aqo_netdev_deinit_rx_channel(struct aqo_device *aqo_dev)
{
	ipa_eth_net_release_channel(aqo_dev->ch_rx.eth_ch);
	aqo_dev->ch_rx.eth_ch = NULL;

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

	return ipa_eth_net_request_event(aqo_dev->ch_rx.eth_ch,
			IPA_ETH_DEV_EV_RX_INT, msi_addr, msi_data);
}

int aqo_netdev_deinit_rx_event(struct aqo_device *aqo_dev)
{
	aqo_log(aqo_dev, "Releasing AQC Rx MSI");

	ipa_eth_net_release_event(aqo_dev->ch_rx.eth_ch,
			IPA_ETH_DEV_EV_RX_INT);

	return 0;
}

int aqo_netdev_start_rx(struct aqo_device *aqo_dev)
{
	int rc;
	struct ipa_eth_channel *ch = aqo_dev->ch_rx.eth_ch;

	rc = ipa_eth_net_moderate_event(ch, IPA_ETH_DEV_EV_RX_INT, 0, 0,
		aqo_dev->rx_int_mod_usecs, aqo_dev->rx_int_mod_usecs * 2);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to set AQC Rx MSI moderation");
		return rc;
	}

	aqo_log(aqo_dev, "AQC Rx MSI moderation set to [%u,%u] usecs",
		aqo_dev->rx_int_mod_usecs, aqo_dev->rx_int_mod_usecs * 2);

	rc = ipa_eth_net_enable_event(ch, IPA_ETH_DEV_EV_RX_INT);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to enable AQC Rx MSI event");
		return rc;
	}

	aqo_log(aqo_dev, "Enabled AQC Rx MSI event");

	rc = ipa_eth_net_enable_channel(ch);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to enable AQC Rx channel");
		ipa_eth_net_disable_event(ch, IPA_ETH_DEV_EV_RX_INT);
		return rc;
	}

	aqo_log(aqo_dev, "Enabled AQC Rx channel");
	aqo_log(aqo_dev, "Started AQC Rx");

	return 0;
}

int aqo_netdev_stop_rx(struct aqo_device *aqo_dev)
{
	int rc_ev, rc_ch;
	struct ipa_eth_channel *ch = aqo_dev->ch_rx.eth_ch;

	rc_ch = ipa_eth_net_disable_channel(ch);
	if (rc_ch)
		aqo_log_err(aqo_dev, "Failed to stop AQC Rx channel");
	else
		aqo_log(aqo_dev, "Stopped AQC Rx channel");

	rc_ev = ipa_eth_net_disable_event(ch, IPA_ETH_DEV_EV_RX_INT);
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
	struct ipa_eth_channel_mem_params mem_params = {
		.desc = {
			.count = aqo_dev->ch_tx.ring_size,
			.hw_map_params = {
				[IPA_ETH_HW_GSI] = {
					.map = true,
					.sym = true,
					.read = true,
					.write = true,
				},
			},
		},
		.buff = {
			.size = aqo_dev->ch_tx.buff_size,
			.hw_map_params = {
				[IPA_ETH_HW_IPA] = {
					.map = true,
					.sym = true,
					.read = false,
					.write = true,
				},
			},
		},
	};
	struct ipa_eth_channel *ch = NULL;

	ch = ipa_eth_net_request_channel(AQO_ETHDEV(aqo_dev),
		IPA_CLIENT_AQC_ETHERNET_CONS, 0, 0, &mem_params);
	if (IS_ERR_OR_NULL(ch)) {
		return ch ? PTR_ERR(ch) : -ENOMEM;
	}

	aqo_log(aqo_dev, "Allocated AQC Tx channel %u", ch->queue);

	ch->od_priv = aqo_dev;
	aqo_dev->ch_tx.eth_ch = ch;

	return 0;
}

int aqo_netdev_deinit_tx_channel(struct aqo_device *aqo_dev)
{
	ipa_eth_net_release_channel(aqo_dev->ch_tx.eth_ch);
	aqo_dev->ch_tx.eth_ch = NULL;

	aqo_log(aqo_dev, "Deallocated AQC Tx channel");

	return 0;
}

int aqo_netdev_init_tx_event(struct aqo_device *aqo_dev)
{
	aqo_log(aqo_dev, "Requesting AQC Tx head pointer write-back");

	return ipa_eth_net_request_event(aqo_dev->ch_tx.eth_ch,
			IPA_ETH_DEV_EV_TX_PTR, aqo_dev->ch_tx.gsi_db.paddr, 0);
}

int aqo_netdev_deinit_tx_event(struct aqo_device *aqo_dev)
{
	aqo_log(aqo_dev, "Releasing AQC Tx head pointer write-back");

	ipa_eth_net_release_event(aqo_dev->ch_tx.eth_ch, IPA_ETH_DEV_EV_TX_PTR);

	return 0;
}

int aqo_netdev_start_tx(struct aqo_device *aqo_dev)
{
	int rc;
	struct ipa_eth_channel *ch = aqo_dev->ch_tx.eth_ch;

	/* Head pointer write-back need to be enabled before enabling channel
	 * in order to avoid missing events
	 */
	rc = ipa_eth_net_enable_event(ch, IPA_ETH_DEV_EV_TX_PTR);
	if (rc) {
		aqo_log_err(aqo_dev,
			"Failed to enable AQC Tx Head pointer write-back");
		return rc;
	}

	aqo_log(aqo_dev, "Enabled AQC Tx Head pointer write-back");

	rc = ipa_eth_net_enable_channel(ch);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to enable AQC Tx channel");
		ipa_eth_net_disable_event(ch, IPA_ETH_DEV_EV_TX_PTR);
		return rc;
	}

	aqo_log(aqo_dev, "Enabled AQC Tx channel");

	return 0;
}

int aqo_netdev_stop_tx(struct aqo_device *aqo_dev)
{
	int rc_ev, rc_ch;
	struct ipa_eth_channel *ch = aqo_dev->ch_tx.eth_ch;

	rc_ch = ipa_eth_net_disable_channel(ch);
	if (rc_ch)
		aqo_log_err(aqo_dev, "Failed to stop AQC Tx channel");
	else
		aqo_log(aqo_dev, "Stopped AQC Tx channel");

	rc_ev = ipa_eth_net_disable_event(ch, IPA_ETH_DEV_EV_TX_PTR);
	if (rc_ch)
		aqo_log_err(aqo_dev,
			"Failed to disable AQC Tx head pointer write-back");
	else
		aqo_log(aqo_dev, "Disabled AQC Tx head pointer write-back");

	if (rc_ev || rc_ch) {
		aqo_log_err(aqo_dev, "Failed to fully stop AQC Tx");
		return -EFAULT;
	}

	aqo_log(aqo_dev, "Stopped AQC Tx");

	return 0;
}

#define AQO_FLT_LOC_CATCH_ALL 40

/**
 * __config_catchall_filter() - Installs or removes the catch-all AQC Rx filter
 *
 * @aqo_dev: aqo_device pointer
 * @insert: If true, inserts rule at %AQO_FLT_LOC_CATCH_ALL. Otherwise removes
 *          any filter installed at the same location.
 *
 * Configure the catch-all filter on AQC NIC. When installed, the catch-all
 * filter directs all incoming traffic to Rx queue associated with offload path.
 * Catch-all filter in Aquantia is implemented using Flex Filter that matches
 * the packet with zero bitmask (effectively matching any packet) and the filter
 * is applied on any packet that is not already matched by other filters (except
 * RSS filter).
 *
 * Returns 0 on success, non-zero otherwise.
 */
static int __config_catchall_filter(struct aqo_device *aqo_dev, bool insert)
{
	struct ethtool_rxnfc rxnfc;
	struct net_device *net_dev = AQO_ETHDEV(aqo_dev)->net_dev;
	struct ipa_eth_channel *ch = aqo_dev->ch_rx.eth_ch;

	if (!net_dev) {
		aqo_log_err(aqo_dev, "Net device information is missing");
		return -EFAULT;
	}

	if (!net_dev->ethtool_ops || !net_dev->ethtool_ops->set_rxnfc) {
		aqo_log_err(aqo_dev,
			"set_rxnfc is not supported by the network driver");
		return -EFAULT;
	}

	if (!ch) {
		aqo_log_err(aqo_dev, "Rx channel is not allocated");
		return -EFAULT;
	}

	memset(&rxnfc, 0, sizeof(rxnfc));

	rxnfc.cmd = insert ? ETHTOOL_SRXCLSRLINS : ETHTOOL_SRXCLSRLDEL;

	rxnfc.fs.ring_cookie = ch->queue;
	rxnfc.fs.location = AQO_FLT_LOC_CATCH_ALL;

	return net_dev->ethtool_ops->set_rxnfc(net_dev, &rxnfc);
}

int aqo_netdev_rxflow_set(struct aqo_device *aqo_dev)
{
	int rc = __config_catchall_filter(aqo_dev, true);

	if (rc)
		aqo_log_err(aqo_dev, "Failed to install catch-all filter");
	else
		aqo_log(aqo_dev, "Installed Rx catch-all filter");

	return rc;
}

int aqo_netdev_rxflow_reset(struct aqo_device *aqo_dev)
{
	int rc = __config_catchall_filter(aqo_dev, false);

	if (rc)
		aqo_log_err(aqo_dev, "Failed to remove catch-all filter");
	else
		aqo_log(aqo_dev, "Removed Rx catch-all filter");

	return rc;
}
