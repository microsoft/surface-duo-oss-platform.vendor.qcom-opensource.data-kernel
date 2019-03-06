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

#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>

#include <linux/irq.h>
#include <linux/msi.h>
#include <linux/interrupt.h>

#include <linux/pci.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include "aqo_i.h"

static LIST_HEAD(aqo_devices);
static DEFINE_MUTEX(aqo_devices_lock);

static const char aqo_driver_name[] = "aqc-ipa";

static const u32 PCI_VENDOR_ID_AQUANTIA = 0x1d6a;

// FIXME: Finalize the list of device ids we will support
static const struct pci_device_id aquantia_pci_ids[] = {
	{ PCI_VDEVICE(AQUANTIA, 0xd107), 0 },
};

static int aqo_pair_device(struct ipa_eth_device *eth_dev,
			    struct aqo_device *aqo_dev)
{
	int rc;

	if (aqo_dev->eth_dev)
		return -EEXIST;

	rc = eth_dev->nd->ops->open_device(eth_dev);
	if (rc) {
		pr_crit("AQC: Failed to open device");
		return rc;
	}

	aqo_dev->eth_dev = eth_dev;

	return 0;
}

static int aqo_pci_probe(struct ipa_eth_device *eth_dev)
{
	int rc = -ENODEV;
	struct aqo_device *aqo_dev;
	struct device *dev = eth_dev->dev;
	struct pci_dev *pci_dev = container_of(dev, struct pci_dev, dev);

	if (dev->bus != &pci_bus_type)
		return -EINVAL;

	if (!pci_match_id(aquantia_pci_ids, pci_dev))
		return -ENODEV;

	mutex_lock(&aqo_devices_lock);

	// link an available aqo_device to the ipa_eth_device
	list_for_each_entry(aqo_dev, &aqo_devices, device_list) {
		rc = aqo_pair_device(eth_dev, aqo_dev);
		if (!rc)
			break;
	}

	mutex_unlock(&aqo_devices_lock);

	if (rc)
		return -ENODEV;

	aqo_dev->pci_dev = pci_dev;

	aqo_dev->regs_base.paddr = pci_resource_start(AQO_PCIDEV(aqo_dev), 0);
	aqo_dev->regs_base.size = pci_resource_len(AQO_PCIDEV(aqo_dev), 0);

	pr_crit("AQC: PCI BAR 0 is at %p, size %zu\n",
			aqo_dev->regs_base.paddr, aqo_dev->regs_base.size);

	aqo_dev->regs_base.vaddr =
		ioremap_nocache(aqo_dev->regs_base.paddr,
			aqo_dev->regs_base.size);

	// FIXME: smmu map for IPA uC/GSI context
	aqo_dev->regs_base.daddr = aqo_dev->regs_base.paddr;

	eth_dev->od_priv = aqo_dev;

	return 0;
}

static void aqo_pci_remove(struct ipa_eth_device *eth_dev)
{
	struct aqo_device *aqo_dev = eth_dev->od_priv;

	eth_dev->od_priv = NULL;

	(void) AQO_NETOPS(aqo_dev)->close_device(eth_dev);

	// unlink aqo_device from ipa_eth_device
	aqo_dev->eth_dev = NULL;
	aqo_dev->pci_dev = NULL;
}

static int aqo_init_tx(struct ipa_eth_device *eth_dev)
{
	int rc = 0;
	struct aqo_device *aqo_dev = eth_dev->od_priv;

	// 1. AQC - Request Tx Ch
	rc = aqo_netdev_init_tx_channel(aqo_dev);
	if (rc) {
		pr_crit("AQC: aqo_netdev_init_tx_channel failed");
		goto err_netdev_init_ch;
	} else {
		pr_crit("AQC: aqo_netdev_init_tx_channel succeeded");
	}

	// 2. IPA - Config IPA EP for Tx
	rc = ipa_eth_ep_init(AQO_ETHDEV(aqo_dev)->ch_tx);
	if (rc) {
		pr_crit("AQC: failed to configure IPA Tx EP");
		goto err_ep_init;
	} else {
		pr_crit("AQC: ipa_eth_ep_init succeeded for tx");
	}

	// 3. GSI - Setup Tx
	rc = aqo_gsi_init_tx(aqo_dev);
	if (rc) {
		pr_crit("AQC: aqo_gsi_init_tx failed");
		goto err_gsi_init;
	} else {
		pr_crit("AQC: aqo_gsi_init_tx succeeded");
	}

	// 4. AQC - Request Tx Ev
	rc = aqo_netdev_init_tx_event(aqo_dev);
	if (rc) {
		pr_crit("AQC: aqo_netdev_init_tx_event failed");
		goto err_netdev_init_ev;
	} else {
		pr_crit("AQC: aqo_netdev_init_tx_event succeeded");
	}

	return 0;

err_netdev_init_ev:
	aqo_gsi_deinit_tx(aqo_dev);
err_gsi_init:
err_ep_init:
	aqo_netdev_deinit_tx_channel(aqo_dev);
err_netdev_init_ch:
	return rc;
}

static int aqo_start_tx(struct ipa_eth_device *eth_dev)
{
	int rc = 0;
	struct aqo_device *aqo_dev = eth_dev->od_priv;

	rc = aqo_netdev_start_tx(aqo_dev);
	if (rc) {
		pr_crit("AQC: aqo_netdev_start_tx failed");
		goto err_netdev_start;
	} else {
		pr_crit("AQC: aqo_netdev_start_tx succeeded");
	}

	rc = aqo_gsi_start_tx(aqo_dev);
	if (rc) {
		pr_crit("AQC: aqo_gsi_start_tx failed");
		goto err_gsi_start;
	} else {
		pr_crit("AQC: aqo_gsi_start_tx succeeded");
	}

	rc = ipa_eth_ep_start(AQO_ETHDEV(aqo_dev)->ch_tx);
	if (rc) {
		pr_crit("AQC: ipa_eth_ep_start failed");
		goto err_ep_start;
	} else {
		pr_crit("AQC: ipa_eth_ep_start succeeded");
	}

	return 0;

err_ep_start:
	aqo_gsi_stop_tx(aqo_dev);
err_gsi_start:
	aqo_netdev_stop_tx(aqo_dev);
err_netdev_start:
	return rc;
}

static int aqo_stop_tx(struct ipa_eth_device *eth_dev)
{
	struct aqo_device *aqo_dev = eth_dev->od_priv;

	// TODO: check return status
	aqo_gsi_stop_tx(aqo_dev);
	aqo_netdev_stop_tx(aqo_dev);

	return 0;
}

static int aqo_deinit_tx(struct ipa_eth_device *eth_dev)
{
	struct aqo_device *aqo_dev = eth_dev->od_priv;

	// TODO: check return status
	aqo_netdev_deinit_tx_event(aqo_dev);
	aqo_gsi_deinit_tx(aqo_dev);
	aqo_netdev_deinit_tx_channel(aqo_dev);

	return 0;
}

static int aqo_init_rx(struct ipa_eth_device *eth_dev)
{
	int rc = 0;
	struct aqo_device *aqo_dev = eth_dev->od_priv;

	// 1. AQC - Request Rx Ch
	rc = aqo_netdev_init_rx_channel(aqo_dev);
	if (rc) {
		pr_crit("AQC: aqo_netdev_init_rx_channel failed");
		goto err_netdev_init_ch;
	} else {
		pr_crit("AQC: aqo_netdev_init_rx_channel succeeded");
	}

	// 2. IPA - Config IPA EP for Rx
	rc = ipa_eth_ep_init(AQO_ETHDEV(aqo_dev)->ch_rx);
	if (rc) {
		pr_crit("AQC: failed to configure IPA Rx EP");
		goto err_ep_init;
	} else {
		pr_crit("AQC: ipa_eth_ep_init succeeded for rx");
	}

	// 3. GSI - Setup Rx
	rc = aqo_gsi_init_rx(aqo_dev);
	if (rc) {
		pr_crit("AQC: aqo_gsi_init_rx failed");
		goto err_gsi_init;
	} else {
		pr_crit("AQC: aqo_gsi_init_rx succeeded");
	}

	// 4. uC - Init
	rc = aqo_proxy_init(aqo_dev);
	if (rc) {
		pr_crit("AQC: aqo_proxy_init failed");
		goto err_proxy_init;
	} else {
		pr_crit("AQC: aqo_proxy_init succeeded");
	}

	// 5. AQC - Request Rx Ev
	rc = aqo_netdev_init_rx_event(aqo_dev);
	if (rc) {
		pr_crit("AQC: aqo_netdev_init_rx_event failed");
		goto err_netdev_init_ev;
	} else {
		pr_crit("AQC: aqo_netdev_init_rx_event succeeded");
	}
	
	return 0;

err_netdev_init_ev:
	aqo_proxy_deinit(aqo_dev);
err_proxy_init:
	aqo_gsi_deinit_rx(aqo_dev);
err_gsi_init:
err_ep_init:
	aqo_netdev_deinit_rx_channel(aqo_dev);
err_netdev_init_ch:
	return rc;
}

static int aqo_start_rx(struct ipa_eth_device *eth_dev)
{
	int rc = 0;
	struct aqo_device *aqo_dev = eth_dev->od_priv;

	rc = ipa_eth_ep_start(AQO_ETHDEV(aqo_dev)->ch_rx);
	if (rc) {
		pr_crit("AQC: ipa_eth_ep_start failed");
		goto err_ep_start;
	} else {
		pr_crit("AQC: ipa_eth_ep_start succeeded");
	}

	rc = aqo_gsi_start_rx(aqo_dev);
	if (rc) {
		pr_crit("AQC: aqo_gsi_start_rx failed");
		goto err_gsi_start;
	} else {
		pr_crit("AQC: aqo_gsi_start_rx succeeded");
	}

	// 5. uC - Setup Rx
	rc = aqo_proxy_start(aqo_dev);
	if (rc) {
		pr_crit("AQC: aqo_proxy_start failed");
		goto err_proxy_start;
	} else {
		pr_crit("AQC: aqo_proxy_start succeeded");
	}

	rc = aqo_netdev_start_rx(aqo_dev);
	if (rc) {
		pr_crit("AQC: aqo_netdev_start_rx failed");
		goto err_netdev_start;
	} else {
		pr_crit("AQC: aqo_netdev_start_rx succeeded");
	}

	return 0;

err_netdev_start:
	aqo_proxy_stop(aqo_dev);
err_proxy_start:
	aqo_gsi_stop_rx(aqo_dev);
err_gsi_start:
err_ep_start:
	return rc;
}

static int aqo_stop_rx(struct ipa_eth_device *eth_dev)
{
	struct aqo_device *aqo_dev = eth_dev->od_priv;

	// TODO: check return status
	aqo_netdev_start_rx(aqo_dev);
	aqo_proxy_stop(aqo_dev);
	aqo_gsi_stop_rx(aqo_dev);

	return 0;
}

static int aqo_deinit_rx(struct ipa_eth_device *eth_dev)
{
	struct aqo_device *aqo_dev = eth_dev->od_priv;

	// TODO: check return status
	aqo_netdev_deinit_rx_event(aqo_dev);
	aqo_proxy_deinit(aqo_dev);
	aqo_gsi_deinit_rx(aqo_dev);
	aqo_netdev_deinit_rx_channel(aqo_dev);

	return 0;
}

static int aqo_get_stats(struct ipa_eth_device *eth_dev,
	struct ipa_eth_offload_stats *stats)
{
	memset(&stats, 0, sizeof(stats));

	return 0;
}

static int aqo_clear_stats(struct ipa_eth_device *eth_dev)
{
	return 0;
}

static struct ipa_eth_offload_ops aqo_offload_ops = {
	.init_tx = aqo_init_tx,
	.start_tx = aqo_start_tx,
	.stop_tx = aqo_stop_tx,
	.deinit_tx = aqo_deinit_tx,

	.init_rx = aqo_init_rx,
	.start_rx = aqo_start_rx,
	.stop_rx = aqo_stop_rx,
	.deinit_rx = aqo_deinit_rx,

	.get_stats = aqo_get_stats,
	.clear_stats = aqo_clear_stats,
};

static struct ipa_eth_bus_ops aqo_bus_ops = {
	.probe = aqo_pci_probe,
	.remove = aqo_pci_remove,
};

static struct ipa_eth_offload_driver aqo_offload_driver = {
	.name = aqo_driver_name,
	.bus = &pci_bus_type,
	.ops = &aqo_offload_ops,
	.bus_ops = &aqo_bus_ops,
};

static int aqo_parse_rx_proxy_uc(struct device_node *np,
		struct aqo_device *aqo_dev)
{
	int rc;
	u32 val32;
	u64 val64;
	const char *key;

	key = "qcom,proxy-msi-addr";
	rc = of_property_read_u64(np, key, &val64);
	if (rc) {
		pr_crit("AQC: %s DT prop missing for %s", key, np->name);
		return rc;
	}

	aqo_dev->ch_rx.proxy.uc_ctx.msi_addr.paddr = (phys_addr_t) val64;

	key = "qcom,proxy-msi-data";
	rc = of_property_read_u32(np, key, &val32);
	if (rc) {
		pr_crit("AQC: %s DT prop missing for %s", key, np->name);
		return rc;
	}

	aqo_dev->ch_rx.proxy.uc_ctx.msi_data = val32;

	aqo_dev->ch_rx.proxy.uc_ctx.valid = true;

	return 0;
}

static int aqo_parse_rx_proxy_host(struct device_node *np,
		struct aqo_device *aqo_dev)
{
	u64 reg;
	const __be32 *preg;

	unsigned int irq;
	unsigned long hwirq;
	struct irq_data *irqd;

	pr_crit("AQC: PARSING HOST PROXY\n");

	irq = irq_of_parse_and_map(np, 0);
	if (!irq) {
		pr_crit("AQC: Failed to parse irq from DT");
		return -EINVAL;
	}

	preg = of_get_address(np, 0, 0, 0);
	if (!preg) {
		pr_crit("AQC: Failed to read 'reg' property");
		return -EFAULT;
	}

	reg = be32_to_cpup(preg);

	irqd = irq_get_irq_data(irq);
	if (!irqd) {
		pr_crit("AQC: Failed to get irq data");
		return -EFAULT;
	}

	hwirq = irqd_to_hwirq(irqd);
	if (!hwirq) {
		pr_crit("AQC: Failed to get hwirq number");
		return -EFAULT;
	}

	if (strcmp(irqd->chip->name, "GIC-0")) {
		pr_crit("AQC: Unsupported interrupt controller");
		return -EFAULT;
	}

	aqo_dev->ch_rx.proxy.host_ctx.irq = irq;
	aqo_dev->ch_rx.proxy.host_ctx.msi_addr.paddr =
		reg + (0x4 * (hwirq / 32));
	aqo_dev->ch_rx.proxy.host_ctx.msi_data = 1 << (hwirq % 32);

	pr_crit("AQC: IRQ=%u, HWIRQ=%u", irq, hwirq);
	pr_crit("AQC: REG=%llx", reg);
	pr_crit("AQC: MSI ADDR=%lx", aqo_dev->ch_rx.proxy.host_ctx.msi_addr.paddr);
	pr_crit("AQC: MSI DATA=%lx", aqo_dev->ch_rx.proxy.host_ctx.msi_data);

	aqo_dev->ch_rx.proxy.host_ctx.valid = true;

	return 0;
}

static int aqo_parse_rx_proxy(struct device_node *np,
		struct aqo_device *aqo_dev)
{
	int rc;
	const char *key;
	const char *str;

	key = "qcom,proxy-agent";
	rc = of_property_read_string(np, key, &str);
	if (rc) {
		pr_crit("AQC: %s missing in %s", key, np->name);
		return rc;
	}

	if (!strcmp(str, "uc"))
		return aqo_parse_rx_proxy_uc(np, aqo_dev);

	if (!strcmp(str, "host"))
		return aqo_parse_rx_proxy_host(np, aqo_dev);

	pr_crit("AQC: unknown proxy agent type: %s", str);

	return -EINVAL;
}

static int aqo_parse_rx_proxies(struct device_node *np,
		struct aqo_device *aqo_dev)
{
	int i;
	int rc = 0;
	struct device_node *pnp;

	for (i = 0; (pnp = of_parse_phandle(np, "qcom,rx-proxy", i)); i++) {
		rc = aqo_parse_rx_proxy(pnp, aqo_dev);
		if (rc) {
			pr_crit("AQC: failed to process rx-proxy %d", i);
			break;
		}
	}

	return rc;
}

static int aqo_parse_dt(struct platform_device *pdev,
	struct aqo_device *aqo_dev)
{
	int rc = 0;
	u32 val32;
	const char *key;
	struct device_node *np = pdev->dev.of_node;

	rc = aqo_parse_rx_proxies(np, aqo_dev);
	if (rc) {
		pr_crit("AQC: failed to parse rx proxies");
		return rc;
	}

	key = "qcom,rx-gsi-mod-count";
	rc = of_property_read_u32(np, key, &val32);
	if (!rc) {
		aqo_dev->ch_rx.gsi_modc = val32;
	} else {
		pr_crit("AQC: %s missing for %s, using default", key, np->name);
		aqo_dev->ch_rx.gsi_modc = AQO_GSI_DEFAULT_RX_MODC;
	}

	key = "qcom,rx-gsi-mod-timer";
	rc = of_property_read_u32(np, key, &val32);
	if (!rc) {
		aqo_dev->ch_rx.gsi_modt = val32;
	} else {
		pr_crit("AQC: %s missing for %s, using default", key, np->name);
		aqo_dev->ch_rx.gsi_modt = AQO_GSI_DEFAULT_RX_MODT;
	}

	key = "qcom,tx-gsi-mod-count";
	rc = of_property_read_u32(np, key, &val32);
	if (!rc) {
		aqo_dev->ch_tx.gsi_modc = val32;
	} else {
		pr_crit("AQC: %s missing for %s, using default", key, np->name);
		aqo_dev->ch_tx.gsi_modc = AQO_GSI_DEFAULT_TX_MODC;
	}

	key = "qcom,tx-gsi-mod-timer";
	rc = of_property_read_u32(np, key, &val32);
	if (!rc) {
		aqo_dev->ch_tx.gsi_modt = val32;
	} else {
		pr_crit("AQC: %s missing for %s, using default", key, np->name);
		aqo_dev->ch_tx.gsi_modt = AQO_GSI_DEFAULT_TX_MODT;
	}

	if (of_property_read_u32(np, "qcom,rx-mod-usecs", &val32))
		val32 = AQO_DEFAULT_RX_MOD_USECS;

	aqo_dev->rx_mod_usecs = clamp_val(val32, AQO_MIN_RX_MOD_USECS,
						AQO_MAX_RX_MOD_USECS);

	if (val32 != aqo_dev->rx_mod_usecs)
		pr_crit("AQC: Rx interrupt moderation clamped at %u usecs\n",
			aqo_dev->rx_mod_usecs);

	aqo_dev->pci_direct = of_property_read_bool(np, "qcom,use-pci-direct");

	return rc;
}

static int aqo_platform_probe(struct platform_device *pdev)
{
	int rc = 0;
	struct aqo_device *aqo_dev = NULL;

	pr_crit("AQC: aqo_platform_probe called");

	aqo_dev = devm_kzalloc(&pdev->dev, sizeof(*aqo_dev), GFP_KERNEL);
	if (!aqo_dev)
		return -ENOMEM;

	aqo_dev->ch_rx.proxy.agent = AQO_PROXY_DEFAULT_AGENT;

	rc = aqo_parse_dt(pdev, aqo_dev);
	if (rc) {
		devm_kfree(&pdev->dev, aqo_dev);
		return rc;
	}

	aqo_dev->pf_dev = pdev;

	platform_set_drvdata(pdev, aqo_dev);

	mutex_lock(&aqo_devices_lock);
	list_add(&aqo_dev->device_list, &aqo_devices);
	mutex_unlock(&aqo_devices_lock);

	return 0;
}

static int aqo_platform_remove(struct platform_device *pdev)
{
	struct aqo_device *aqo_dev = platform_get_drvdata(pdev);

	devm_kfree(&pdev->dev, aqo_dev);

	return 0;
}

static struct of_device_id aqo_platform_match[] = {
	{ .compatible = "qcom,aqc-ipa" },
	{},
};

static struct platform_driver aqo_platform_driver = {
	.probe = aqo_platform_probe,
	.remove = aqo_platform_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = aqo_driver_name,
		.of_match_table = aqo_platform_match,
	},
};

int __init aqo_init(void)
{
	int rc;

	pr_crit("AQC: aqo_init called");
	
	rc = platform_driver_register(&aqo_platform_driver);
	if (rc) {
		pr_crit("AQC: unable to register offload driver");
		return rc;
	}

	return ipa_eth_register_offload_driver(&aqo_offload_driver);
}
module_init(aqo_init);

void __exit aqo_exit(void)
{
	pr_crit("AQC: aqo_exit called");

	ipa_eth_unregister_offload_driver(&aqo_offload_driver);
	platform_driver_unregister(&aqo_platform_driver);
}
module_exit(aqo_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("IPA Tether Driver for Aquantia Ethernet Adapter");
