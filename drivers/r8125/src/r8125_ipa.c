/* Copyright (c) 2020 The Linux Foundation. All rights reserved.
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

#include <linux/module.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/version.h>
#include <linux/device.h>
#include <linux/pm_wakeup.h>

#define IPA_ETH_NET_DRIVER
#define IPA_ETH_OFFLOAD_DRIVER

#include <linux/ipa_eth.h>
#include <linux/ipa.h>
#include <linux/msm_ipa.h>
#include <linux/if_vlan.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/msi.h>
#include <linux/ethtool.h>

#include "r8125_lib.h"
#include "r8125_ipa.h"
#include "r8125.h"

#define RTL8125_IPA_INST_ID 0
#define RTL8125_IPA_NUM_OF_PIPES 2
#define RTL8125_IPA_DESC_SIZE 32
#define RTL8125_IPA_NUM_TX_DESC 1024

/* To address IPA ring index overflow/wraparound limitations,
 * Rx Desc size cannot be a power of 2.
 */
#define RTL8125_IPA_NUM_RX_DESC 1023
#define RTL8125_BAR_MMIO 2

/* Minimum power-of-2 size to hold 1500 payload */
#define RTL8125_IPA_RX_BUF_SIZE 2048
#define RTL8125_IPA_TX_BUF_SIZE 2048

/* Mitigation timer = 150uS
 * Timer unit is 2048nS.
 * 150uS/2048nS = 75 (approx)
 */
#define RTL8125_IPA_MITIGATION_TIME_RX 75
#define RTL8125_IPA_MITIGATION_TIME_TX 75

#define RTL_LOG_PREFIX "[RTL8125] "

static bool enable_ipa_offload;

/*setting mode 0 so that the module parameter cannot be modified from sysfs*/
module_param(enable_ipa_offload, bool, 0000);
MODULE_PARM_DESC(enable_ipa_offload, "Enable IPA Offload");

static const struct pci_device_id *pci_device_ids;

#define to_eth_dev(rtl_dev) rtl_dev->eth_dev
#define to_ndev(rtl_dev) to_eth_dev(rtl_dev)->net_dev
#define to_dev(rtl_dev) to_eth_dev(rtl_dev)->dev

#define rtl_log(rtl_dev, fmt, args...) \
	do { \
		struct rtl8125_device *__rtl_dev = rtl_dev; \
		struct ipa_eth_device *eth_dev = \
					__rtl_dev ? __rtl_dev->eth_dev : NULL; \
		struct device *dev = eth_dev ? eth_dev->dev : NULL; \
		struct net_device *net_dev = \
					eth_dev ? eth_dev->net_dev : NULL; \
		const char *netdev_name = \
				net_dev ? net_dev->name : "<unpaired>"; \
		\
		dev_dbg(dev, RTL_LOG_PREFIX "(%s) " fmt "\n", \
			netdev_name, ## args); \
		ipa_eth_ipc_log(RTL_LOG_PREFIX "(%s) " fmt, \
				netdev_name, ##args); \
	} while (0)

#define rtl_log_err(rtl_dev, fmt, args...) \
	do { \
		struct rtl8125_device *__rtl_dev = rtl_dev; \
		struct ipa_eth_device *eth_dev = \
					__rtl_dev ? __rtl_dev->eth_dev : NULL; \
		struct device *dev = eth_dev ? eth_dev->dev : NULL; \
		struct net_device *net_dev = \
					eth_dev ? eth_dev->net_dev : NULL; \
		const char *netdev_name = \
				net_dev ? net_dev->name : "<unpaired>"; \
		\
		dev_err(dev, RTL_LOG_PREFIX "(%s) ERROR: " fmt "\n", \
			netdev_name, ## args); \
		ipa_eth_ipc_log(RTL_LOG_PREFIX "(%s) ERROR: " fmt, \
				netdev_name, ##args); \
	} while (0)

#ifdef DEBUG
#define rtl_log_bug(rtl_dev, fmt, args...) \
		do { \
			rtl_log_err(rtl_dev, "BUG: " fmt, ##args); \
			WARN_ON(); \
		} while (0)

#define rtl_log_dbg(rtl_dev, fmt, args...) \
	do { \
		struct rtl8125_device *__rtl_dev = rtl_dev; \
		struct ipa_eth_device *eth_dev = \
					__rtl_dev ? __rtl_dev->eth_dev : NULL; \
		struct device *dev = eth_dev ? eth_dev->dev : NULL; \
		struct net_device *net_dev = \
					eth_dev ? eth_dev->net_dev : NULL; \
		const char *netdev_name = \
				net_dev ? net_dev->name : "<unpaired>"; \
		\
		dev_dbg(dev, RTL_LOG_PREFIX "(%s) " fmt "\n", \
			netdev_name, ## args); \
		ipa_eth_ipc_dbg(RTL_LOG_PREFIX "(%s) DEBUG: " fmt, \
				netdev_name, ##args); \
	} while (0)
#else
#define rtl_log_bug(rtl_dev, fmt, args...) \
	do { \
		rtl_log_err(rtl_dev, "BUG: " fmt, ##args); \
		dump_stack(); \
	} while (0)

#define rtl_log_dbg(rtl_dev, fmt, args...) \
	do { \
		struct rtl8125_device *__rtl_dev = rtl_dev; \
		struct ipa_eth_device *eth_dev = \
					__rtl_dev ? __rtl_dev->eth_dev : NULL; \
		struct net_device *net_dev = \
					eth_dev ? eth_dev->net_dev : NULL; \
		const char *netdev_name = \
				net_dev ? net_dev->name : "<unpaired>"; \
		\
		ipa_eth_ipc_dbg(RTL_LOG_PREFIX "(%s) DEBUG: " fmt, \
				netdev_name, ##args); \
	} while (0)
#endif /* DEBUG */

struct rtl8125_regs {
	ktime_t begin_ktime;
	ktime_t end_ktime;
	u64 duration_ns;
};

union rtl8125_ipa_eth_hdr {
	struct ethhdr l2;
	struct vlan_ethhdr vlan;
};

struct rtl8125_ch_info {
	dma_addr_t ipa_db_dma_addr;
	struct rtl8125_ring *ring;
	struct ipa_eth_client_pipe_info pipe_info;
};

struct rtl8125_device {
	struct ipa_eth_device *eth_dev;

	struct ipa_eth_client eth_client;
	struct ipa_eth_intf_info intf;
	struct rtl8125_regs regs_save;
	struct rtl8125_ch_info rx_info;
	struct rtl8125_ch_info tx_info;
	union rtl8125_ipa_eth_hdr hdr_v4;
	union rtl8125_ipa_eth_hdr hdr_v6;

	struct wakeup_source rtl8125_ws;
	phys_addr_t mmio_phys_addr;
	size_t mmio_size;
	u64 exception_packets;

	struct rtl8125_private *rtl8125_tp;
};

static void rtl8125_ipa_notify_cb(void *priv,
					enum ipa_dp_evt_type evt,
					unsigned long data)
{
	struct rtl8125_device *rtl_dev = priv;
	struct sk_buff *skb = (struct sk_buff *)data;
	struct net_device *net_dev = to_ndev(rtl_dev);

	if (evt != IPA_RECEIVE)
		return;

	if (!rtl_dev || !skb) {
		rtl_log_err(rtl_dev, "Null Param: pdata %p, skb %p",
				rtl_dev, skb);
		return;
	}

	skb->protocol = eth_type_trans(skb, net_dev);
	rtl_dev->exception_packets++;

	netif_rx_ni(skb);
}

static int rtl8125_match_pci(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);

	if (dev->bus != &pci_bus_type) {
		rtl_log(NULL, "Device bus type is not PCI");
		return -EINVAL;
	}

	if (!pci_match_id(pci_device_ids, pci_dev)) {
		rtl_log(NULL, "Device PCI ID is not compatible");
		return -ENODEV;
	}

	return 0;
}

static int rtl8125_pair(struct ipa_eth_device *eth_dev)
{
	int rc = 0;
	struct device *dev = eth_dev->dev;
	struct rtl8125_device *rtl_dev = eth_dev->od_priv;

	rtl_log(NULL, "Pairing started");

	if (!eth_dev || !dev) {
		rtl_log_err(NULL, "Invalid ethernet device structure");
		return -EFAULT;
	}

	rc = rtl8125_match_pci(dev);
	if (rc) {
		rtl_log(NULL, "Failed to parse device");
		goto err_parse;
	}

	rtl_dev->mmio_phys_addr = pci_resource_start(to_pci_dev(dev),
							RTL8125_BAR_MMIO);
	rtl_dev->mmio_size = pci_resource_len(to_pci_dev(dev),
							RTL8125_BAR_MMIO);

	rtl_log(rtl_dev, "Successfully paired");

	return 0;

err_parse:
	return rc;
}

static void rtl8125_unpair(struct ipa_eth_device *eth_dev)
{
	struct rtl8125_device *rtl_dev = eth_dev->od_priv;

	rtl_log(rtl_dev, "Successfully unpaired");
}

static int rtl8125_init_tx(struct ipa_eth_device *eth_dev)
{
	return 0;
}

static void __iomem *rtl8125_msix_bar(struct pci_dev *pdev)
{
	struct msi_desc *msi;

	if (!pdev->msix_enabled)
		return NULL;

	msi = list_first_entry(dev_to_msi_list(&pdev->dev),
		struct msi_desc, list);

	return msi->mask_base;
}

static void rtl8125_log_msix(struct pci_dev *pdev)
{
	int i;
	void __iomem *desc = rtl8125_msix_bar(pdev);

	if (!desc)
		return;

	for (i = 0; i < 32; i++) {
		void __iomem *msix = desc + (i * PCI_MSIX_ENTRY_SIZE);

		rtl_log(NULL, "MSI[%d]: %x %x %x %x",
				i,
				readl_relaxed(msix + PCI_MSIX_ENTRY_UPPER_ADDR),
				readl_relaxed(msix + PCI_MSIX_ENTRY_LOWER_ADDR),
				readl_relaxed(msix + PCI_MSIX_ENTRY_DATA),
				readl_relaxed(msix + PCI_MSIX_ENTRY_VECTOR_CTRL)
				);
	}
}

static void rtl8125_deinit_pipe_info(
				struct ipa_eth_client_pipe_info *pipe_info)
{
	kfree(pipe_info->info.data_buff_list);
	sg_free_table(pipe_info->info.transfer_ring_sgt);
	kfree(pipe_info->info.transfer_ring_sgt);
	memset(pipe_info, 0, sizeof(*pipe_info));
}

static int rtl8125_init_pipe_info(
				struct rtl8125_device *rtl_dev,
				struct rtl8125_ch_info *ch_info)
{
	struct device *dev = to_dev(rtl_dev);
	struct rtl8125_ring *rtl_ring = ch_info->ring;
	struct ipa_eth_client_pipe_info *pipe_info = &ch_info->pipe_info;
	struct ipa_eth_realtek_setup_info *rtl_info =
					&pipe_info->info.client_info.rtk;
	int ret, i;

	INIT_LIST_HEAD(&pipe_info->link);
	if (rtl_ring->direction == RTL8125_CH_DIR_RX)
		pipe_info->dir = IPA_ETH_PIPE_DIR_RX;
	else if (rtl_ring->direction == RTL8125_CH_DIR_TX)
		pipe_info->dir = IPA_ETH_PIPE_DIR_TX;
	else {
		rtl_log_err(rtl_dev, "Failed to set direction for pipe info");
		return -EINVAL;
	}

	pipe_info->client_info = &rtl_dev->eth_client;
	pipe_info->info.is_transfer_ring_valid = true;
	pipe_info->info.transfer_ring_base = rtl_ring->desc_daddr;
	pipe_info->info.transfer_ring_size = rtl_ring->desc_size;
	pipe_info->info.fix_buffer_size = rtl_ring->buff_size;
	pipe_info->info.transfer_ring_sgt =
		kzalloc(sizeof(pipe_info->info.transfer_ring_sgt), GFP_KERNEL);

	if (!pipe_info->info.transfer_ring_sgt) {
		rtl_log_err(rtl_dev,
				"Failed to alloc transfer ring sgt buffer");
		goto ring_sgt_alloc_err;
	}

	ret = dma_get_sgtable(dev, pipe_info->info.transfer_ring_sgt,
					rtl_ring->desc_addr,
					rtl_ring->desc_daddr,
					rtl_ring->desc_size);
	if (ret)
		goto sgtable_err;

	pipe_info->info.data_buff_list_size = rtl_ring->ring_size;
	pipe_info->info.data_buff_list =
				kcalloc(rtl_ring->ring_size,
					sizeof(*pipe_info->info.data_buff_list),
					GFP_KERNEL);

	if (!pipe_info->info.data_buff_list) {
		rtl_log_err(rtl_dev, "Failed to alloc data buff list");
		goto buff_alloc_err;
	}

	for (i = 0; i < rtl_ring->ring_size; i++) {
		pipe_info->info.data_buff_list[i].iova =
			rtl_ring->bufs[i].dma_addr;
		pipe_info->info.data_buff_list[i].pa =
			page_to_phys(vmalloc_to_page(rtl_ring->bufs[i].addr)) |
			((phys_addr_t)rtl_ring->bufs[i].addr & ~PAGE_MASK);
	}

	rtl_info->bar_addr = rtl_dev->mmio_phys_addr;
	rtl_info->bar_size = rtl_dev->mmio_size;
	rtl_info->queue_number = rtl_ring->queue_num;

	if (rtl_ring->direction == RTL8125_CH_DIR_RX) {
		pipe_info->info.notify = rtl8125_ipa_notify_cb;
		pipe_info->info.priv = rtl_dev;
	} else if (rtl_ring->direction == RTL8125_CH_DIR_TX) {
		rtl_info->dest_tail_ptr_offs =
			SW_TAIL_PTR0_8125 + rtl_ring->queue_num * 4;
	}

	return 0;

buff_alloc_err:
	sg_free_table(pipe_info->info.transfer_ring_sgt);
sgtable_err:
	kfree(pipe_info->info.transfer_ring_sgt);
ring_sgt_alloc_err:
	memset(pipe_info, 0, sizeof(*pipe_info));
	return -EINVAL;
}

static void rtl8125_ipa_init_intf_info_eth_hdr(
				struct rtl8125_device *rtl_dev)
{
	struct ipa_eth_intf_info *intf_info = &rtl_dev->intf;
	struct net_device *net_dev = to_ndev(rtl_dev);

	memcpy(&rtl_dev->hdr_v4.l2.h_source, net_dev->dev_addr, ETH_ALEN);
	rtl_dev->hdr_v4.l2.h_proto = htons(ETH_P_IP);
	intf_info->hdr[0].hdr = (u8 *)&rtl_dev->hdr_v4.l2;
	intf_info->hdr[0].hdr_len = ETH_HLEN;
	intf_info->hdr[0].hdr_type = IPA_HDR_L2_ETHERNET_II;

	memcpy(&rtl_dev->hdr_v6.l2.h_source, net_dev->dev_addr, ETH_ALEN);
	rtl_dev->hdr_v6.l2.h_proto = htons(ETH_P_IPV6);
	intf_info->hdr[1].hdr = (u8 *)&rtl_dev->hdr_v6.l2;
	intf_info->hdr[1].hdr_len = ETH_HLEN;
	intf_info->hdr[1].hdr_type = IPA_HDR_L2_ETHERNET_II;
}

static void rtl8125_ipa_init_intf_info_vlan_hdr(
				struct rtl8125_device *rtl_dev)
{
	struct ipa_eth_intf_info *intf_info = &rtl_dev->intf;
	struct net_device *net_dev = to_ndev(rtl_dev);

	memcpy(&rtl_dev->hdr_v4.vlan.h_source, net_dev->dev_addr, ETH_ALEN);
	rtl_dev->hdr_v4.vlan.h_vlan_proto = htons(ETH_P_8021Q);
	rtl_dev->hdr_v4.vlan.h_vlan_encapsulated_proto = htons(ETH_P_IP);
	intf_info->hdr[0].hdr = (u8 *)&rtl_dev->hdr_v4.vlan;
	intf_info->hdr[0].hdr_len = VLAN_ETH_HLEN;
	intf_info->hdr[0].hdr_type = IPA_HDR_L2_802_1Q;

	memcpy(&rtl_dev->hdr_v6.vlan.h_source, net_dev->dev_addr, ETH_ALEN);
	rtl_dev->hdr_v6.vlan.h_vlan_proto = htons(ETH_P_8021Q);
	rtl_dev->hdr_v6.vlan.h_vlan_encapsulated_proto = htons(ETH_P_IPV6);
	intf_info->hdr[1].hdr = (u8 *)&rtl_dev->hdr_v6.vlan;
	intf_info->hdr[1].hdr_len = VLAN_ETH_HLEN;
	intf_info->hdr[1].hdr_type = IPA_HDR_L2_802_1Q;
}

static void rtl8125_ipa_deinit_intf_info_hdrs(
				struct ipa_eth_intf_info *intf_info)
{
	memset(&intf_info->hdr[1].hdr, 0, sizeof(struct ipa_eth_hdr_info));
	memset(&intf_info->hdr[0].hdr, 0, sizeof(struct ipa_eth_hdr_info));
}

static int rtl8125_ipa_init_intf_info_hdrs(struct rtl8125_device *rtl_dev)
{
	bool ipa_vlan_mode = false;

	if (ipa_is_vlan_mode(IPA_VLAN_IF_EMAC, &ipa_vlan_mode)) {
		rtl_log_err(rtl_dev, "Failed to get vlan mode");
		return -EINVAL;
	}

	if (!ipa_vlan_mode)
		rtl8125_ipa_init_intf_info_eth_hdr(rtl_dev);
	else
		rtl8125_ipa_init_intf_info_vlan_hdr(rtl_dev);

	return 0;
}

static int rtl8125_send_ipa_ecm_msg(struct rtl8125_device *rtl_dev,
						bool connected)
{
	struct ipa_ecm_msg msg;
	struct net_device *net_dev = to_ndev(rtl_dev);

	strlcpy(msg.name, net_dev->name, sizeof(msg.name));
	msg.ifindex = net_dev->ifindex;

	return connected ?
		 ipa_eth_client_conn_evt(&msg) :
		 ipa_eth_client_disconn_evt(&msg);
}

static void rtl8125_teardown_event(struct rtl8125_device *rtl_dev,
					struct rtl8125_ch_info *ch_info)
{
	struct device *dev = to_dev(rtl_dev);

	rtl8125_disable_event(ch_info->ring);
	rtl8125_release_event(ch_info->ring);
	dma_unmap_resource(dev, ch_info->ipa_db_dma_addr,
				sizeof(u32),
				DMA_FROM_DEVICE,
				0);
	ch_info->ipa_db_dma_addr = 0;
}

static void rtl8125_teardown_events(struct rtl8125_device *rtl_dev)
{
	rtl_log_dbg(rtl_dev, "Tearing down rx and tx events");

	rtl8125_teardown_event(rtl_dev, &rtl_dev->rx_info);
	rtl8125_teardown_event(rtl_dev, &rtl_dev->tx_info);
}

static int rtl8125_setup_event(struct rtl8125_device *rtl_dev,
					enum rtl8125_channel_dir direction)
{
	struct device *dev = to_dev(rtl_dev);
	struct rtl8125_ch_info *ch_info;
	int delay;

	if (direction == RTL8125_CH_DIR_RX) {
		ch_info = &rtl_dev->rx_info;
		delay = RTL8125_IPA_MITIGATION_TIME_RX;
	} else if (direction == RTL8125_CH_DIR_TX) {
		ch_info = &rtl_dev->tx_info;
		delay = RTL8125_IPA_MITIGATION_TIME_TX;
	} else {
		rtl_log_err(rtl_dev, "Unknown data path direction");
		return -EINVAL;
	}

	ch_info->ipa_db_dma_addr = dma_map_resource(dev,
						ch_info->pipe_info.info.db_pa,
						sizeof(u32),
						DMA_FROM_DEVICE,
						0);

	if (dma_mapping_error(dev, ch_info->ipa_db_dma_addr)) {
		rtl_log_err(rtl_dev, "DMA mapping error for IPA DB");
		return -ENOMEM;
	}

	rtl_log_dbg(rtl_dev, "phy db-addr = %pap, dma addr = %pad",
		&ch_info->pipe_info.info.db_pa, &ch_info->ipa_db_dma_addr);

	if (rtl8125_request_event(ch_info->ring, MSIX_event_type,
					ch_info->ipa_db_dma_addr,
					ch_info->pipe_info.info.db_val)) {
		rtl_log_err(rtl_dev, "Failed in setting event");
		goto err_req_event;
	}

	if (rtl8125_set_ring_intr_mod(ch_info->ring, delay)) {
		rtl_log_err(rtl_dev, "Failed to set mitigation time");
		goto err_event;
	}

	if (rtl8125_enable_event(ch_info->ring)) {
		rtl_log_err(rtl_dev, "Failed to enable event");
		goto err_event;
	}

	return 0;

err_event:
	rtl8125_release_event(ch_info->ring);
err_req_event:
	dma_unmap_resource(dev, ch_info->ipa_db_dma_addr,
					sizeof(u32),
					DMA_FROM_DEVICE,
					0);
	ch_info->ipa_db_dma_addr = 0;
	return -EINVAL;
}

static int rtl8125_setup_events(struct rtl8125_device *rtl_dev)
{
	rtl_log_dbg(rtl_dev, "Setting rx and tx events");

	if (rtl8125_setup_event(rtl_dev, RTL8125_CH_DIR_RX)) {
		rtl_log_err(rtl_dev, "Failed to setup rx event");
		goto err_rx_event;
	}

	if (rtl8125_setup_event(rtl_dev, RTL8125_CH_DIR_TX)) {
		rtl_log_err(rtl_dev, "Failed to setup tx event");
		goto err_tx_event;
	}

	return 0;

err_tx_event:
	rtl8125_teardown_event(rtl_dev, &rtl_dev->rx_info);
err_rx_event:
	return -EINVAL;
}

static void rtl8125_teardown_ring(struct rtl8125_ch_info *ch_info)
{
	rtl8125_disable_ring(ch_info->ring);
	rtl8125_deinit_pipe_info(&ch_info->pipe_info);
	rtl8125_release_ring(ch_info->ring);
	ch_info->ring = NULL;
}

static void rtl8125_teardown_rings(struct rtl8125_device *rtl_dev)
{
	rtl_log_dbg(rtl_dev, "Tearing down rx and tx rings");

	rtl8125_teardown_ring(&rtl_dev->rx_info);
	rtl8125_teardown_ring(&rtl_dev->tx_info);
}

static int rtl8125_setup_ring(struct rtl8125_device *rtl_dev,
					enum rtl8125_channel_dir direction,
					unsigned int ring_size,
					unsigned int buff_size,
					struct rtl8125_ch_info *ch_info)
{
	struct net_device *ndev = to_ndev(rtl_dev);

	ch_info->ring = rtl8125_request_ring(ndev, ring_size,
						buff_size,
						direction,
						RTL8125_CONTIG_BUFS,
						NULL);

	if (!ch_info->ring) {
		rtl_log_err(rtl_dev, "Request ring failed");
		goto err_req_ring;
	}

	rtl_log_dbg(rtl_dev, "ring size = %x, buff size = %x, desc size = %x",
		ch_info->ring->ring_size,
		ch_info->ring->buff_size,
		ch_info->ring->desc_size);

	rtl_log_dbg(rtl_dev, "dma addr = %pad, physical addr = %pap",
		&ch_info->ring->desc_daddr,
		&ch_info->ring->desc_paddr);

	if (rtl8125_init_pipe_info(rtl_dev, ch_info)) {
		rtl_log_err(rtl_dev, "Failed to allocate pipe info");
		goto err_init_pipe_info;
	}

	if (rtl8125_enable_ring(ch_info->ring)) {
		rtl_log_err(rtl_dev, "Failed to enable ring");
		goto err_enable_ring;
	}

	return 0;

err_enable_ring:
	rtl8125_deinit_pipe_info(&ch_info->pipe_info);
err_init_pipe_info:
	rtl8125_release_ring(ch_info->ring);
	ch_info->ring = NULL;
err_req_ring:
	return -EINVAL;
}

static int rtl8125_setup_rings(struct rtl8125_device *rtl_dev)
{
	rtl_log_dbg(rtl_dev, "Setting rx and tx rings");

	if (rtl8125_setup_ring(rtl_dev, RTL8125_CH_DIR_RX,
					RTL8125_IPA_NUM_RX_DESC,
					RTL8125_IPA_RX_BUF_SIZE,
					&rtl_dev->rx_info)) {
		rtl_log_err(rtl_dev, "Failed to setup rx ring");
		goto err;
	}

	if (rtl8125_setup_ring(rtl_dev, RTL8125_CH_DIR_TX,
					RTL8125_IPA_NUM_TX_DESC,
					RTL8125_IPA_TX_BUF_SIZE,
					&rtl_dev->tx_info)) {
		rtl_log_err(rtl_dev, "Failed to setup tx ring");
		goto err_tx_ring;
	}

	return 0;

err_tx_ring:
	rtl8125_teardown_ring(&rtl_dev->rx_info);
err:
	return -EINVAL;
}

static int rtl8125_vote_ipa_bw(struct rtl8125_device *rtl_dev)
{
	struct ipa_eth_perf_profile profile;

	rtl_log_dbg(rtl_dev, "Voting IPA bandwidth");

	memset(&profile, 0, sizeof(profile));
	profile.max_supported_bw_mbps = SPEED_2500;

	if (ipa_eth_client_set_perf_profile(&rtl_dev->eth_client, &profile)) {
		rtl_log_err(rtl_dev, "Failed to set voting on bandwidth");
		return -EINVAL;
	}

	return 0;
}

static void rtl8125_del_client_pipe_list(struct rtl8125_device *rtl_dev)
{
	struct ipa_eth_client *eth_client = &rtl_dev->eth_client;
	struct ipa_eth_client_pipe_info *pipe_info, *tmp;

	list_for_each_entry_safe(pipe_info, tmp, &eth_client->pipe_list, link)
		list_del(&pipe_info->link);
}

static void rtl8125_teardown_ipa_pipes(struct rtl8125_device *rtl_dev)
{
	if (ipa_eth_client_disconn_pipes(&rtl_dev->eth_client))
		rtl_log_err(rtl_dev, "Failed to disconnect IPA pipes");

	rtl8125_del_client_pipe_list(rtl_dev);
}

static int rtl8125_setup_ipa_pipes(struct rtl8125_device *rtl_dev)
{
	struct ipa_eth_client_pipe_info *rx_pipe_info =
						&rtl_dev->rx_info.pipe_info;
	struct ipa_eth_client_pipe_info *tx_pipe_info =
						&rtl_dev->tx_info.pipe_info;

	rtl_log_dbg(rtl_dev, "Setting up IPA pipes");

	INIT_LIST_HEAD(&rtl_dev->eth_client.pipe_list);
	list_add(&rx_pipe_info->link, &rtl_dev->eth_client.pipe_list);
	list_add(&tx_pipe_info->link, &rtl_dev->eth_client.pipe_list);

	if (ipa_eth_client_conn_pipes(&rtl_dev->eth_client) != 0) {
		rtl_log_err(rtl_dev, "Failed to connect ipa pipes");
		rtl8125_del_client_pipe_list(rtl_dev);
		return -EINVAL;
	}

	if (rtl8125_vote_ipa_bw(rtl_dev))
		goto err_vote_bw;

	return 0;

err_vote_bw:
	rtl8125_teardown_ipa_pipes(rtl_dev);
	return -EINVAL;
}

static void rtl8125_teardown_ipa_intf(struct rtl8125_device *rtl_dev)
{
	struct ipa_eth_intf_info *intf_info = &rtl_dev->intf;

	if (rtl8125_send_ipa_ecm_msg(rtl_dev, false))
		rtl_log_err(rtl_dev, "Failed to send ecm");

	if (ipa_eth_client_unreg_intf(intf_info))
		rtl_log_err(rtl_dev, "Failed to unregister interface");

	kfree(intf_info->pipe_hdl_list);
	intf_info->pipe_hdl_list = NULL;

	rtl8125_ipa_deinit_intf_info_hdrs(intf_info);
	memset(intf_info, 0, sizeof(*intf_info));
}

static int rtl8125_setup_ipa_intf(struct rtl8125_device *rtl_dev)
{
	struct net_device *net_dev = to_ndev(rtl_dev);
	struct ipa_eth_intf_info *intf_info = &rtl_dev->intf;

	intf_info->netdev_name = net_dev->name;
	if (rtl8125_ipa_init_intf_info_hdrs(rtl_dev))
		goto err_fill_hdrs;

	intf_info->pipe_hdl_list =
		kcalloc(RTL8125_IPA_NUM_OF_PIPES,
			sizeof(*intf_info->pipe_hdl_list),
			GFP_KERNEL);
	if (!intf_info->pipe_hdl_list) {
		rtl_log_err(rtl_dev, "Failed to alloc pipe handle list");
		goto err_alloc_pipe_hndls;
	}

	intf_info->pipe_hdl_list[0] = rtl_dev->rx_info.pipe_info.pipe_hdl;
	intf_info->pipe_hdl_list[1] = rtl_dev->tx_info.pipe_info.pipe_hdl;
	intf_info->pipe_hdl_list_size = RTL8125_IPA_NUM_OF_PIPES;
	if (ipa_eth_client_reg_intf(intf_info)) {
		rtl_log_err(rtl_dev, "Failed to register IPA interface");
		goto err_reg_intf;
	}

	if (rtl8125_send_ipa_ecm_msg(rtl_dev, true)) {
		rtl_log_err(rtl_dev, "Failed to send ecm");
		goto err_ecm_conn;
	}

	return 0;

err_ecm_conn:
	ipa_eth_client_unreg_intf(intf_info);
err_reg_intf:
	kfree(intf_info->pipe_hdl_list);
	intf_info->pipe_hdl_list = NULL;
err_alloc_pipe_hndls:
	rtl8125_ipa_deinit_intf_info_hdrs(intf_info);
err_fill_hdrs:
	memset(intf_info, 0, sizeof(*intf_info));
	return -EINVAL;
}

static int rtl8125_start_tx(struct ipa_eth_device *eth_dev)
{
	struct rtl8125_device *rtl_dev = eth_dev->od_priv;
	struct net_device *net_dev = to_ndev(rtl_dev);

	rtl_log(rtl_dev, "Starting IPA offload");

	if (rtl8125_setup_rings(rtl_dev))
		goto err_rtl_rings;

	if (rtl8125_setup_ipa_pipes(rtl_dev))
		goto err_ipa_pipes;

	if (rtl8125_setup_events(rtl_dev))
		goto err_rtl_events;

	if (rtl8125_setup_ipa_intf(rtl_dev))
		goto err_ipa_intf;

	if (rtl8125_rss_redirect(net_dev, 0, rtl_dev->rx_info.ring)) {
		rtl_log_err(rtl_dev, "Failed to redirect RSS");
		goto err_rss_redirect;
	}

	rtl8125_log_msix(to_pci_dev(eth_dev->dev));

	return 0;

err_rss_redirect:
	rtl8125_teardown_ipa_intf(rtl_dev);
err_ipa_intf:
	rtl8125_teardown_events(rtl_dev);
err_rtl_events:
	rtl8125_teardown_ipa_pipes(rtl_dev);
err_ipa_pipes:
	rtl8125_teardown_rings(rtl_dev);
err_rtl_rings:
	return -EINVAL;
}

static int rtl8125_stop_tx(struct ipa_eth_device *eth_dev)
{
	struct rtl8125_device *rtl_dev = eth_dev->od_priv;

	rtl_log(rtl_dev, "Stopping IPA offload");

	rtl8125_rss_reset(eth_dev->net_dev);

	rtl8125_teardown_ipa_intf(rtl_dev);

	rtl8125_teardown_events(rtl_dev);

	rtl8125_teardown_ipa_pipes(rtl_dev);

	rtl8125_teardown_rings(rtl_dev);

	return 0;
}

static int rtl8125_deinit_tx(struct ipa_eth_device *eth_dev)
{
	return 0;
}

static int rtl8125_init_rx(struct ipa_eth_device *eth_dev)
{
	return 0;
}

static int rtl8125_start_rx(struct ipa_eth_device *eth_dev)
{
	return 0;
}

static int rtl8125_stop_rx(struct ipa_eth_device *eth_dev)
{
	return 0;
}

static int rtl8125_deinit_rx(struct ipa_eth_device *eth_dev)
{
	return 0;
}

size_t rtl8125_regs_save(struct rtl8125_device *rtl_dev,
				struct rtl8125_regs *regs)
{

	regs->begin_ktime = ktime_get();

	regs->end_ktime = ktime_get();

	regs->duration_ns =
		ktime_to_ns(ktime_sub(regs->end_ktime, regs->begin_ktime));

	return 0;
}

static int rtl8125_save_regs(struct ipa_eth_device *eth_dev,
		void **regs, size_t *size)
{
	return 0;
}

static int rtl8125_ipa_open_device(struct ipa_eth_device *eth_dev)
{
	struct rtl8125_device *rtl_dev;

	if (!eth_dev || !eth_dev->dev) {
		rtl_log_err(rtl_dev, "Invalid ethernet device structure");
		return -EFAULT;
	}

	rtl_dev = kzalloc(sizeof(*rtl_dev), GFP_KERNEL);
	if (!rtl_dev) {
		rtl_log_err(NULL, "Failed to alloc rtl device");
		return -ENOMEM;
	}

	rtl_dev->eth_client.inst_id = RTL8125_IPA_INST_ID;
	rtl_dev->eth_client.client_type = IPA_ETH_CLIENT_RTK8125B;
	rtl_dev->eth_client.traffic_type = IPA_ETH_PIPE_BEST_EFFORT;
	rtl_dev->eth_client.priv = rtl_dev;

	eth_dev->od_priv = rtl_dev;
	eth_dev->net_dev = rtl8125_get_netdev(eth_dev->dev);
	rtl_dev->eth_dev = eth_dev;
	rtl_dev->rtl8125_tp = netdev_priv(eth_dev->net_dev);

	wakeup_source_init(&rtl_dev->rtl8125_ws, "rtl8125-ipa");
	__pm_stay_awake(&rtl_dev->rtl8125_ws);

	return 0;
}

static void rtl8125_ipa_close_device(struct ipa_eth_device *eth_dev)
{
	struct rtl8125_device *rtl_dev = eth_dev->od_priv;

	__pm_relax(&rtl_dev->rtl8125_ws);
	wakeup_source_trash(&rtl_dev->rtl8125_ws);

	eth_dev->od_priv = NULL;
	eth_dev->net_dev = NULL;

	kzfree(rtl_dev);
}

static struct ipa_eth_channel *rtl8125_ipa_request_channel(
	struct ipa_eth_device *eth_dev, enum ipa_eth_channel_dir dir,
	unsigned long events, unsigned long features,
	const struct ipa_eth_channel_mem_params *mem_params)
{
	return NULL;
}

static void rtl8125_ipa_release_channel(struct ipa_eth_channel *ch)
{
}

static int rtl8125_ipa_enable_channel(struct ipa_eth_channel *ch)
{
	return -EINVAL;
}

static int rtl8125_ipa_disable_channel(struct ipa_eth_channel *ch)
{
	return -EINVAL;
}

static int rtl8125_ipa_request_event(struct ipa_eth_channel *ch,
				 unsigned long ipa_event,
				 phys_addr_t addr, u64 data)
{
	return -EINVAL;
}

static void rtl8125_ipa_release_event(struct ipa_eth_channel *ch,
				  unsigned long ipa_event)
{
}

static int rtl8125_ipa_enable_event(struct ipa_eth_channel *ch,
				unsigned long event)
{
	return -EINVAL;
}

static int rtl8125_ipa_disable_event(struct ipa_eth_channel *ch,
				 unsigned long event)
{
	return -EINVAL;
}

int rtl8125_ipa_moderate_event(struct ipa_eth_channel *ch,
					unsigned long event,
					u64 min_count, u64 max_count,
					u64 min_usecs, u64 max_usecs)
{
	return -EINVAL;
}

static int rtl8125_ipa_receive_skb(struct ipa_eth_device *eth_dev,
				struct sk_buff *skb, bool in_napi)
{
	return -EINVAL;
}

static int rtl8125_ipa_transmit_skb(struct ipa_eth_device *eth_dev,
				struct sk_buff *skb)
{
	return -EINVAL;
}

struct ipa_eth_net_ops rtl8125_net_ops = {
	.open_device = rtl8125_ipa_open_device,
	.close_device = rtl8125_ipa_close_device,
	.request_channel = rtl8125_ipa_request_channel,
	.release_channel = rtl8125_ipa_release_channel,
	.enable_channel = rtl8125_ipa_enable_channel,
	.disable_channel = rtl8125_ipa_disable_channel,
	.request_event = rtl8125_ipa_request_event,
	.release_event = rtl8125_ipa_release_event,
	.enable_event = rtl8125_ipa_enable_event,
	.disable_event = rtl8125_ipa_disable_event,
	.moderate_event = rtl8125_ipa_moderate_event,
	.receive_skb = rtl8125_ipa_receive_skb,
	.transmit_skb = rtl8125_ipa_transmit_skb,
};

static struct ipa_eth_net_driver rtl8125_net_driver = {
	.events =
		IPA_ETH_DEV_EV_RX_INT |
		IPA_ETH_DEV_EV_TX_INT |
		IPA_ETH_DEV_EV_TX_PTR,
	.features =
		IPA_ETH_DEV_F_L2_CSUM |
		IPA_ETH_DEV_F_L3_CSUM |
		IPA_ETH_DEV_F_TCP_CSUM |
		IPA_ETH_DEV_F_UDP_CSUM |
		IPA_ETH_DEV_F_LSO |
		IPA_ETH_DEV_F_LRO |
		IPA_ETH_DEV_F_VLAN |
		IPA_ETH_DEV_F_MODC |
		IPA_ETH_DEV_F_MODT |
		IPA_ETH_DEV_F_IPA_API,
	.bus = &pci_bus_type,
	.ops = &rtl8125_net_ops,
};

static struct ipa_eth_offload_ops rtl8125_offload_ops = {
	.pair = rtl8125_pair,
	.unpair = rtl8125_unpair,

	.init_tx = rtl8125_init_tx,
	.start_tx = rtl8125_start_tx,
	.stop_tx = rtl8125_stop_tx,
	.deinit_tx = rtl8125_deinit_tx,

	.init_rx = rtl8125_init_rx,
	.start_rx = rtl8125_start_rx,
	.stop_rx = rtl8125_stop_rx,
	.deinit_rx = rtl8125_deinit_rx,
	.save_regs = rtl8125_save_regs,
};

static struct ipa_eth_offload_driver rtl8125_offload_driver = {
	.bus = &pci_bus_type,
	.ops = &rtl8125_offload_ops,
};

int rtl8125_ipa_register(struct pci_driver *pdrv)
{
	if (!enable_ipa_offload)
		return 0;

	if (!pci_device_ids)
		pci_device_ids = pdrv->id_table;

	if (!rtl8125_net_driver.name)
		rtl8125_net_driver.name = pdrv->name;

	if (!rtl8125_net_driver.driver)
		rtl8125_net_driver.driver = &pdrv->driver;

	if (ipa_eth_register_net_driver(&rtl8125_net_driver) != 0)
		return -EINVAL;

	if (!rtl8125_offload_driver.name)
		rtl8125_offload_driver.name = pdrv->name;

	if (ipa_eth_register_offload_driver(&rtl8125_offload_driver) != 0) {
		ipa_eth_unregister_net_driver(&rtl8125_net_driver);
		return -EINVAL;
	}

	return 0;
}

void rtl8125_ipa_unregister(struct pci_driver *pdrv)
{
	if (!enable_ipa_offload)
		return;

	ipa_eth_unregister_offload_driver(&rtl8125_offload_driver);
	ipa_eth_unregister_net_driver(&rtl8125_net_driver);
}
