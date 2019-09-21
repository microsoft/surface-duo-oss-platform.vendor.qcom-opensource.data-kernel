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

#ifndef __AQO_I_H__
#define __AQO_I_H__

#include <linux/pci.h>
#include <linux/msm_gsi.h>

#define IPA_ETH_OFFLOAD_DRIVER
#include <linux/ipa_eth.h>

#include "aqo_regs.h"

#define AQO_DRIVER_NAME "aqc-ipa"
#define AQO_DRIVER_VERSION "0.1.0"

#ifdef CONFIG_AQC_IPA_DEBUG
#define DEBUG
#endif

#if defined(CONFIG_AQC_IPA_PROXY_UC)
  #define AQO_PROXY_DEFAULT_AGENT AQO_PROXY_UC
#elif defined(CONFIG_AQC_IPA_PROXY_HOST)
  #define AQO_PROXY_DEFAULT_AGENT AQO_PROXY_HOST
#else
  #error "CONFIG_AQC_IPA_PROXY_* not set"
#endif

#define AQO_GSI_MODC_MIN 0
#define AQO_GSI_MODC_MAX (BIT(8) - 1)
#define AQO_GSI_DEFAULT_RX_MODC 0
#define AQO_GSI_DEFAULT_TX_MODC 0

#define AQO_GSI_MODT_MIN 0
#define AQO_GSI_MODT_MAX (BIT(16) - 1)
/* GSI moderation timer is defined in number of cycles where 32 cycles = 1 ms */
#define AQO_GSI_DEFAULT_RX_MODT 32
#define AQO_GSI_DEFAULT_TX_MODT 32

#define AQO_AQC_RX_INT_MOD_USECS_MIN 0
#define AQO_AQC_RX_INT_MOD_USECS_MAX 511
#define AQO_AQC_RX_INT_MOD_USECS_DEFAULT 250

#define AQO_GSI_TX_WRB_MODC_MIN 0
#define AQO_GSI_TX_WRB_MODC_MAX (BIT_ULL(32) - 1)
#define AQO_GSI_TX_WRB_MODC_DEFAULT 0

#define AQO_AQC_RING_SZ_MIN 8
#define AQO_AQC_RING_SZ_MAX 8192
#define AQO_AQC_RING_SZ_ALIGN 8
#define AQO_AQC_RING_SZ_DEFAULT 128

#define AQO_AQC_BUFF_SZ_MIN 1024
#define AQO_AQC_BUFF_SZ_MAX 16384
#define AQO_AQC_BUFF_SZ_ALIGN 1024
#define AQO_AQC_BUFF_SZ_DEFAULT 2048

#define AQO_GSI_RING_SZ_MIN 1
#define AQO_GSI_RING_SZ_MAX (BIT(12) - 1)
#define AQO_GSI_RING_SZ_ALIGN 1

#define AQO_PCI_DIRECT_MASK BIT_ULL(40)
#define AQO_PCI_DIRECT_SET(val) (val | AQO_PCI_DIRECT_MASK)
#define AQO_PCI_DIRECT_CLEAR(val) (val & ~AQO_PCI_DIRECT_MASK)

enum aqo_proxy_agent {
	AQO_PROXY_UC,
	AQO_PROXY_HOST,
	AQO_PROXY_MAX_AGENTS,
};

enum aqo_proxy_mode {
	AQO_PROXY_MODE_COUNTER,
	AQO_PROXY_MODE_HEADPTR,
	AQO_PROXY_MODE_MAX,
};

struct aqo_proxy_host_context
{
	bool valid;

	unsigned int irq;

	u32 msi_data;
	struct ipa_eth_resource msi_addr;

	void __iomem *aqc_base;

	void __iomem *aqc_hp;
	void __iomem *gsi_db;

	dma_addr_t desc_dbase;
	void *desc_vbase;
	u32 max_head;
	u32 head;

	u32 counter;

	struct task_struct *thread;
};

struct aqo_proxy_uc_context
{
	bool valid;

	u32 msi_data;
	struct ipa_eth_resource msi_addr;

	dma_addr_t aqc_base;

	u64 per_base;
	u8 aqc_ch;
	u8 gsi_ch;
};

struct aqo_proxy {
	enum aqo_proxy_agent agent;
	enum aqo_proxy_mode mode;

	struct aqo_proxy_uc_context uc_ctx;
	struct aqo_proxy_host_context host_ctx;
};

#ifndef __LITTLE_ENDIAN_BITFIELD
#error Big endian bit field is not supported
#endif

struct aqo_gsi_rx_ch_scratch {
	/* Word 1 */
	unsigned buff_mem_lsb:32;

	/* Word 2 */
	unsigned buff_msm_msb:8;
	unsigned unused_1:8;
	unsigned buff_size_log2:16;

	/* Word 3 */
	unsigned head_ptr_lsb:32;

	/* Word 4 */
	unsigned head_ptr_msb:9;
	unsigned unused_2:23;
} __packed;

struct aqo_gsi_tx_ch_scratch {
	/* Word 1 */
	unsigned unused_1:32;

	/* Word 2 */
	unsigned unused_2:16;
	unsigned buff_size_log2:16;
} __packed;

struct aqo_gsi_tx_evt_ring_scratch {
	/* Word 1 */
	unsigned head_ptr_wrb_thresh:32;
} __packed;

union aqo_gsi_ch_scratch {
	union gsi_channel_scratch scratch;
	struct aqo_gsi_rx_ch_scratch rx;
	struct aqo_gsi_tx_ch_scratch tx;
} __packed;

union aqo_gsi_evt_ring_scratch {
	union gsi_evt_scratch scratch;
	struct aqo_gsi_tx_evt_ring_scratch tx;
} __packed;

struct aqo_gsi_config {
	struct gsi_chan_props ch_props;
	union aqo_gsi_ch_scratch ch_scratch;
	phys_addr_t ch_db_addr;
	u64 ch_db_val;

	struct gsi_evt_ring_props evt_ring_props;
	union aqo_gsi_evt_ring_scratch evt_ring_scratch;
	phys_addr_t evt_ring_db_addr;
	u64 evt_ring_db_val;
};

struct aqo_channel {
	struct aqo_proxy proxy;

	u32 ring_size;
	u32 buff_size;

	u8 gsi_ch;
	struct ipa_eth_resource gsi_db;

	u32 gsi_modc;
	u32 gsi_modt;

	struct aqo_gsi_config gsi_config;

	struct ipa_eth_channel *eth_ch;
};

enum aqo_device_states {
	AQO_DEV_S_RX_GSI_INIT,
	AQO_DEV_S_RX_GSI_START,
	AQO_DEV_S_TX_GSI_INIT,
	AQO_DEV_S_TX_GSI_START,
	AQO_DEV_S_IN_SSR,
};

struct aqo_device {
	struct list_head device_list;

	struct ipa_eth_device *eth_dev;
	struct ipa_eth_resource regs_base;

	struct aqo_channel ch_rx;
	struct aqo_channel ch_tx;

	u32 rx_int_mod_usecs;
	u32 tx_wrb_mod_count;

	bool pci_direct;

	unsigned long state;
	struct mutex ssr_mutex;

	struct aqo_regs regs_save;
};

#define AQO_ETHDEV(aqo_dev) ((aqo_dev)->eth_dev)
#define AQO_NETDRV(aqo_dev) (AQO_ETHDEV(aqo_dev)->nd)
#define AQO_NETOPS(aqo_dev) (AQO_NETDRV(aqo_dev)->ops)
#define AQO_DEV(aqo_dev) (AQO_ETHDEV(aqo_dev)->dev)

/* uC */

int aqo_uc_init_peripheral(u64 per_base);
int aqo_uc_setup_channel(bool tx, u8 aqc_ch, u8 gsi_ch);
int aqo_uc_teardown_channel(u8 gsi_ch);
int aqo_uc_deinit_peripheral(void);

/* GSI */

int aqo_gsi_init_rx(struct aqo_device *aqo_dev);
int aqo_gsi_deinit_rx(struct aqo_device *aqo_dev);

int aqo_gsi_start_rx(struct aqo_device *aqo_dev);
int aqo_gsi_stop_rx(struct aqo_device *aqo_dev);

int aqo_gsi_init_tx(struct aqo_device *aqo_dev);
int aqo_gsi_deinit_tx(struct aqo_device *aqo_dev);

int aqo_gsi_start_tx(struct aqo_device *aqo_dev);
int aqo_gsi_stop_tx(struct aqo_device *aqo_dev);

int aqo_gsi_prepare_ssr(struct aqo_device *aqo_dev);
int aqo_gsi_complete_ssr(struct aqo_device *aqo_dev);

/* AQC Netdev */

int aqo_netdev_init_rx_channel(struct aqo_device *aqo_dev);
int aqo_netdev_deinit_rx_channel(struct aqo_device *aqo_dev);

int aqo_netdev_init_rx_event(struct aqo_device *aqo_dev);
int aqo_netdev_deinit_rx_event(struct aqo_device *aqo_dev);

int aqo_netdev_start_rx(struct aqo_device *aqo_dev);
int aqo_netdev_stop_rx(struct aqo_device *aqo_dev);

int aqo_netdev_init_tx_channel(struct aqo_device *aqo_dev);
int aqo_netdev_deinit_tx_channel(struct aqo_device *aqo_dev);

int aqo_netdev_init_tx_event(struct aqo_device *aqo_dev);
int aqo_netdev_deinit_tx_event(struct aqo_device *aqo_dev);

int aqo_netdev_start_tx(struct aqo_device *aqo_dev);
int aqo_netdev_stop_tx(struct aqo_device *aqo_dev);

int aqo_netdev_rxflow_set(struct aqo_device *aqo_dev);
int aqo_netdev_rxflow_reset(struct aqo_device *aqo_dev);

bool aqo_proxy_valid(struct aqo_device *aqo_dev);

int aqo_proxy_init(struct aqo_device *aqo_dev);
int aqo_proxy_start(struct aqo_device *aqo_dev);
int aqo_proxy_stop(struct aqo_device *aqo_dev);
int aqo_proxy_deinit(struct aqo_device *aqo_dev);

size_t aqo_regs_save(struct aqo_device *aqo_dev, struct aqo_regs *regs);

#define AQO_LOG_PREFIX "[aqo] "

#define aqo_log(aqo_dev, fmt, args...) \
	do { \
		struct aqo_device *__aqo_dev = aqo_dev; \
		struct ipa_eth_device *eth_dev = \
					__aqo_dev ? __aqo_dev->eth_dev : NULL; \
		struct device *dev = eth_dev ? eth_dev->dev : NULL; \
		struct net_device *net_dev = \
					eth_dev ? eth_dev->net_dev : NULL; \
		const char *netdev_name = \
				net_dev ? net_dev->name : "<unpaired>"; \
		\
		dev_dbg(dev, AQO_LOG_PREFIX "(%s) " fmt "\n", \
			netdev_name, ## args); \
		ipa_eth_ipc_log(AQO_LOG_PREFIX "(%s) " fmt, \
				netdev_name, ##args); \
	} while (0)

#define aqo_log_err(aqo_dev, fmt, args...) \
	do { \
		struct aqo_device *__aqo_dev = aqo_dev; \
		struct ipa_eth_device *eth_dev = \
					__aqo_dev ? __aqo_dev->eth_dev : NULL; \
		struct device *dev = eth_dev ? eth_dev->dev : NULL; \
		struct net_device *net_dev = \
					eth_dev ? eth_dev->net_dev : NULL; \
		const char *netdev_name = \
				net_dev ? net_dev->name : "<unpaired>"; \
		\
		dev_err(dev, AQO_LOG_PREFIX "(%s) " "ERROR: " fmt "\n", \
			netdev_name, ## args); \
		ipa_eth_ipc_log(AQO_LOG_PREFIX "(%s) " "ERROR: " fmt, \
				netdev_name, ##args); \
	} while (0)

#ifdef DEBUG
#define aqo_log_bug(aqo_dev, fmt, args...) \
	do { \
		aqo_log_err(aqo_dev, "BUG: " fmt, ##args); \
		BUG(); \
	} while (0)

#define aqo_log_dbg(aqo_dev, fmt, args...) \
	do { \
		struct aqo_device *__aqo_dev = aqo_dev; \
		struct ipa_eth_device *eth_dev = \
					__aqo_dev ? __aqo_dev->eth_dev : NULL; \
		struct device *dev = eth_dev ? eth_dev->dev : NULL; \
		struct net_device *net_dev = \
					eth_dev ? eth_dev->net_dev : NULL; \
		const char *netdev_name = \
				net_dev ? net_dev->name : "<unpaired>"; \
		\
		dev_dbg(dev, AQO_LOG_PREFIX "(%s) " fmt "\n", \
			netdev_name, ## args); \
		ipa_eth_ipc_dbg(AQO_LOG_PREFIX "(%s) " "DEBUG: " fmt, \
				netdev_name, ##args); \
	} while (0)
#else
#define aqo_log_bug(aqo_dev, fmt, args...) \
	do { \
		aqo_log_err(aqo_dev, "BUG: " fmt, ##args); \
		dump_stack(); \
	} while (0)

#define aqo_log_dbg(aqo_dev, fmt, args...) \
	do { \
		struct aqo_device *__aqo_dev = aqo_dev; \
		struct ipa_eth_device *eth_dev = \
					__aqo_dev ? __aqo_dev->eth_dev : NULL; \
		struct net_device *net_dev = \
					eth_dev ? eth_dev->net_dev : NULL; \
		const char *netdev_name = \
				net_dev ? net_dev->name : "<unpaired>"; \
		\
		ipa_eth_ipc_dbg(AQO_LOG_PREFIX "(%s) " "DEBUG: " fmt, \
				netdev_name, ##args); \
	} while (0)
#endif /* DEBUG */

#endif /* __AQO_I_H__ */