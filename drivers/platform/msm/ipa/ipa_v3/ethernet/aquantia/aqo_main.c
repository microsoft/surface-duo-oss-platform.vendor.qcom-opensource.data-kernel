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

static const char aqo_driver_name[] = AQO_DRIVER_NAME;

static const u32 PCI_VENDOR_ID_AQUANTIA = 0x1d6a;

static const struct pci_device_id aquantia_pci_ids[] = {
	{ PCI_VDEVICE(AQUANTIA, 0xd107), 0 },
	{ PCI_VDEVICE(AQUANTIA, 0x07b1), 0 },
	{ PCI_VDEVICE(AQUANTIA, 0x87b1), 0 },
	{ PCI_VDEVICE(AQUANTIA, 0x00b1), 0 },
	{ PCI_VDEVICE(AQUANTIA, 0x80b1), 0 },
};

static const struct of_device_id aquantia_of_matches[] = {
	{ .compatible = "aquantia,aqc-107" },
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
		aqo_log_err(aqo_dev,
			"%s DT prop is missing for %s", key, np->name);
		return rc;
	}

	aqo_dev->ch_rx.proxy.uc_ctx.msi_addr.paddr = (phys_addr_t) val64;

	key = "qcom,proxy-msi-data";
	rc = of_property_read_u32(np, key, &val32);
	if (rc) {
		aqo_log_err(aqo_dev,
			"%s DT prop is missing for %s", key, np->name);
		return rc;
	}

	aqo_dev->ch_rx.proxy.uc_ctx.msi_data = val32;

	aqo_dev->ch_rx.proxy.uc_ctx.valid = true;

	aqo_log(aqo_dev, "Rx proxy via IPA uC is available");

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

	irq = irq_of_parse_and_map(np, 0);
	if (!irq) {
		aqo_log_err(aqo_dev,"IRQ information missing in DT");
		return -EINVAL;
	}

	preg = of_get_address(np, 0, 0, 0);
	if (!preg) {
		aqo_log_err(aqo_dev,"Interrupt pending register missing in DT");
		return -EFAULT;
	}

	reg = be32_to_cpup(preg);

	irqd = irq_get_irq_data(irq);
	if (!irqd) {
		aqo_log_err(aqo_dev, "Failed to extract IRQ data for irq %u",
				irq);
		return -EFAULT;
	}

	hwirq = irqd_to_hwirq(irqd);
	if (!hwirq) {
		aqo_log_err(aqo_dev, "Failed to extract HW IRQ for irq %u",
				irq);
		return -EFAULT;
	}

	if (strcmp(irqd->chip->name, "GIC-0")) {
		aqo_log_err(aqo_dev,
				"Unsupported IRQ chip %s", irqd->chip->name);
		return -EFAULT;
	}

	aqo_dev->ch_rx.proxy.host_ctx.irq = irq;
	aqo_dev->ch_rx.proxy.host_ctx.msi_addr.paddr =
		reg + (0x4 * (hwirq / 32));
	aqo_dev->ch_rx.proxy.host_ctx.msi_data = 1 << (hwirq % 32);

	aqo_log_dbg(aqo_dev,
			"MSI irq: %u, hwirq: %u, ispendr: 0x%llx",
			irq, hwirq, reg);
	aqo_log_dbg(aqo_dev,
			"MSI addr: 0x%lx, data: 0x%lx",
			aqo_dev->ch_rx.proxy.host_ctx.msi_addr.paddr,
			aqo_dev->ch_rx.proxy.host_ctx.msi_data);

	aqo_dev->ch_rx.proxy.host_ctx.valid = true;

	aqo_log(aqo_dev, "Rx proxy via Host is available");

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
		aqo_log_err(aqo_dev,
			"%s DT prop is missing for %s", key, np->name);
		return rc;
	}

	if (!strcmp(str, "uc"))
		return aqo_parse_rx_proxy_uc(np, aqo_dev);

	if (!strcmp(str, "host"))
		return aqo_parse_rx_proxy_host(np, aqo_dev);

	aqo_log_err(aqo_dev, "Invalid proxy agent type %s", str);

	return -EINVAL;
}

static int aqo_parse_rx_proxies(struct device_node *np,
		struct aqo_device *aqo_dev)
{
	int i;
	int rc;
	const char *key;
	const char *str;
	struct device_node *pnp;

	rc = 0;
	for (i = 0; (pnp = of_parse_phandle(np, "qcom,rx-proxy", i)); i++) {
		int r = aqo_parse_rx_proxy(pnp, aqo_dev);

		if (r) {
			aqo_log_err(aqo_dev, "Failed to parse proxy %d", i);
			rc |= r;
		}
	}
	if (rc)
		return rc;

	key = "qcom,rx-proxy-mode";
	rc = of_property_read_string(np, key, &str);
	if (rc) {
		str = "counter";
		aqo_log(aqo_dev,
			"%s DT prop is missing for %s, using '%s' as value",
			key, np->name, str);
	}

	if (!strcmp(str, "counter")) {
		aqo_dev->ch_rx.proxy.mode = AQO_PROXY_MODE_COUNTER;
	} else if (!strcmp(str, "head-pointer")) {
		aqo_dev->ch_rx.proxy.mode = AQO_PROXY_MODE_HEADPTR;
	} else {
		aqo_log_err(aqo_dev, "Rx proxy mode %s is invalid", str);
		return -EINVAL;
	}

	if (!aqo_proxy_valid(aqo_dev)) {
		aqo_log_err(aqo_dev,
			"Default proxy agent config is not valid");
		return -EINVAL;
	}

	return 0;
}

static int aqo_parse_rx_ring_size(struct device_node *np,
		struct aqo_device *aqo_dev)
{
	int rc;
	u32 val32;
	const char *key;

	key = "qcom,rx-ring-size";
	rc = of_property_read_u32(np, key, &val32);
	if (rc) {
		val32 = AQO_AQC_RING_SZ_DEFAULT;
		aqo_log(aqo_dev,
			"DT prop %s is missing for %s, using default %lu",
			key, np->name, val32);
	}

	aqo_dev->ch_rx.ring_size = val32;
	aqo_dev->ch_rx.ring_size =
		clamp_val(aqo_dev->ch_rx.ring_size,
			AQO_AQC_RING_SZ_MIN, AQO_AQC_RING_SZ_MAX);
	aqo_dev->ch_rx.ring_size =
		clamp_val(aqo_dev->ch_rx.ring_size,
			AQO_GSI_RING_SZ_MIN, AQO_GSI_RING_SZ_MAX);
	aqo_dev->ch_rx.ring_size =
		rounddown(aqo_dev->ch_rx.ring_size, AQO_AQC_RING_SZ_ALIGN);
	aqo_dev->ch_rx.ring_size =
		rounddown(aqo_dev->ch_rx.ring_size, AQO_GSI_RING_SZ_ALIGN);

	if (aqo_dev->ch_rx.ring_size != val32)
		aqo_log(aqo_dev,
			"Rx ring size clamped/realigned to %u",
			aqo_dev->ch_rx.ring_size);

	return 0;
}

static int aqo_parse_rx_buff_size(struct device_node *np,
		struct aqo_device *aqo_dev)
{
	int rc;
	u32 val32;
	const char *key;

	key = "qcom,rx-buff-size";
	rc = of_property_read_u32(np, key, &val32);
	if (rc) {
		val32 = AQO_AQC_BUFF_SZ_DEFAULT;
		aqo_log(aqo_dev,
			"DT prop %s is missing for %s, using default %lu",
			key, np->name, val32);
	}

	aqo_dev->ch_rx.buff_size = val32;
	aqo_dev->ch_rx.buff_size =
		clamp_val(aqo_dev->ch_rx.buff_size,
			AQO_AQC_BUFF_SZ_MIN, AQO_AQC_BUFF_SZ_MAX);
	aqo_dev->ch_rx.buff_size =
		rounddown(aqo_dev->ch_rx.buff_size, AQO_AQC_BUFF_SZ_ALIGN);

	/* GSI receives buffer size as log2(buff_size) in a 16-bit field which
	 * implies the following theoretical limitations:
	 *   - Max buffer size of 2 ^ 65535 (which should never reach)
	 *   - Buffer size to be a power of 2
	 */
	aqo_dev->ch_rx.buff_size =
		roundup_pow_of_two(aqo_dev->ch_rx.buff_size);

	if (aqo_dev->ch_rx.buff_size > AQO_AQC_BUFF_SZ_MAX)
		aqo_dev->ch_rx.buff_size =
			rounddown_pow_of_two(aqo_dev->ch_rx.buff_size);

	if (aqo_dev->ch_rx.buff_size != val32)
		aqo_log(aqo_dev,
			"Rx buff size clamped/realigned to %u",
			aqo_dev->ch_rx.buff_size);

	return 0;
}

static int aqo_parse_rx_int_mod(struct device_node *np,
		struct aqo_device *aqo_dev)
{
	int rc;
	u32 val32;
	const char *key;

	key = "qcom,rx-int-mod-usecs";
	rc = of_property_read_u32(np, key, &val32);
	if (rc) {
		val32 = AQO_AQC_RX_INT_MOD_USECS_DEFAULT;
		aqo_log(aqo_dev,
			"DT prop %s is missing for %s, using default %lu",
			key, np->name, val32);
	}

	aqo_dev->rx_int_mod_usecs =
		clamp_val(val32,
			AQO_AQC_RX_INT_MOD_USECS_MIN,
			AQO_AQC_RX_INT_MOD_USECS_MAX);

	if (aqo_dev->rx_int_mod_usecs != val32)
		aqo_log(aqo_dev,
			"Rx interrupt moderation clamped at %u usecs",
			aqo_dev->rx_int_mod_usecs);

	return 0;
}

static int aqo_parse_rx_gsi_modc(struct device_node *np,
		struct aqo_device *aqo_dev)
{
	int rc;
	u32 val32;
	const char *key;

	/* Set invalid modc so that we will know when we set one below */
	aqo_dev->ch_rx.gsi_modc = aqo_dev->ch_rx.ring_size + 1;

	key = "qcom,rx-gsi-mod-count";
	rc = of_property_read_u32(np, key, &val32);
	if (!rc) {
		aqo_dev->ch_rx.gsi_modc = val32;
		aqo_dev->ch_rx.gsi_modc =
			clamp_val(aqo_dev->ch_rx.gsi_modc,
				AQO_GSI_MODC_MIN, AQO_GSI_MODC_MAX);
		aqo_dev->ch_rx.gsi_modc =
			clamp_val(aqo_dev->ch_rx.gsi_modc,
				0, aqo_dev->ch_rx.ring_size);
		if (aqo_dev->ch_rx.gsi_modc != val32) {
			aqo_log(aqo_dev, "Rx GSI MODC clamped at %u",
				aqo_dev->ch_rx.gsi_modc);
		}
	}

	key = "qcom,rx-gsi-mod-pc";
	rc = of_property_read_u32(np, key, &val32);
	if (!rc) {
		u32 pc = clamp_val(val32, 0, 100);
		u32 modc = (aqo_dev->ch_rx.ring_size * pc) / 100;

		if (pc != val32) {
			aqo_log(aqo_dev,
				"Rx GSI MODC PC clamped at %u percent", pc);
		}

		/* If both qcom,rx-gsi-mod-count and qcom,rx-gsi-mod-pc are
		 * specified, choose the lowest among them.
		 */
		if (modc < aqo_dev->ch_rx.gsi_modc) {
			aqo_log(aqo_dev,
				"Setting Rx GSI MODC to %u (%u %% of %u)",
				modc, pc, aqo_dev->ch_rx.ring_size);
			aqo_dev->ch_rx.gsi_modc =
				clamp_val(modc,
					AQO_GSI_MODC_MIN, AQO_GSI_MODC_MAX);
			if (aqo_dev->ch_rx.gsi_modc != modc) {
				aqo_log(aqo_dev, "Rx GSI MODC clamped at %u",
					aqo_dev->ch_rx.gsi_modc);
			}
		}
	}

	/* Set default modc if a DT prop was not found above */
	if (aqo_dev->ch_rx.gsi_modc > aqo_dev->ch_rx.ring_size) {
		aqo_dev->ch_rx.gsi_modc = AQO_GSI_DEFAULT_RX_MODC;
		aqo_log(aqo_dev, "Setting Rx GSI MODC to default %u",
			aqo_dev->ch_rx.gsi_modc);
	}

	return 0;
}

static int aqo_parse_rx_gsi_modt(struct device_node *np,
		struct aqo_device *aqo_dev)
{
	int rc;
	u32 val32;
	const char *key;

	key = "qcom,rx-gsi-mod-timer";
	rc = of_property_read_u32(np, key, &val32);
	if (!rc) {
		aqo_dev->ch_rx.gsi_modt = clamp_val(val32,
					AQO_GSI_MODT_MIN, AQO_GSI_MODT_MAX);
		if (aqo_dev->ch_rx.gsi_modt != val32) {
			aqo_log(aqo_dev, "Rx GSI MODT clamped at %u cycles",
				aqo_dev->ch_rx.gsi_modt);
		}
	} else {
		aqo_dev->ch_rx.gsi_modt = AQO_GSI_DEFAULT_RX_MODT;
		aqo_log(aqo_dev,
			"DT prop %s is missing for %s, using default %lu",
			key, np->name, aqo_dev->ch_rx.gsi_modt);
	}

	return 0;
}

static int aqo_parse_rx_props(struct device_node *np,
		struct aqo_device *aqo_dev)
{
	return aqo_parse_rx_ring_size(np, aqo_dev) ||
		aqo_parse_rx_buff_size(np, aqo_dev) ||
		aqo_parse_rx_int_mod(np, aqo_dev) ||
		aqo_parse_rx_gsi_modc(np, aqo_dev) ||
		aqo_parse_rx_gsi_modt(np, aqo_dev);
}

static int aqo_parse_tx_ring_size(struct device_node *np,
		struct aqo_device *aqo_dev)
{
	int rc;
	u32 val32;
	const char *key;

	key = "qcom,tx-ring-size";
	rc = of_property_read_u32(np, key, &val32);
	if (rc) {
		val32 = AQO_AQC_RING_SZ_DEFAULT;
		aqo_log(aqo_dev,
			"DT prop %s is missing for %s, using default %lu",
			key, np->name, val32);
	}

	aqo_dev->ch_tx.ring_size = val32;
	aqo_dev->ch_tx.ring_size =
		clamp_val(aqo_dev->ch_tx.ring_size,
			AQO_AQC_RING_SZ_MIN, AQO_AQC_RING_SZ_MAX);
	aqo_dev->ch_tx.ring_size =
		clamp_val(aqo_dev->ch_tx.ring_size,
			AQO_GSI_RING_SZ_MIN, AQO_GSI_RING_SZ_MAX);
	aqo_dev->ch_tx.ring_size =
		rounddown(aqo_dev->ch_tx.ring_size, AQO_AQC_RING_SZ_ALIGN);
	aqo_dev->ch_tx.ring_size =
		rounddown(aqo_dev->ch_tx.ring_size, AQO_GSI_RING_SZ_ALIGN);

	if (aqo_dev->ch_tx.ring_size != val32)
		aqo_log(aqo_dev,
			"Tx ring size clamped/realigned to %u",
			aqo_dev->ch_tx.ring_size);

	return 0;
}

static int aqo_parse_tx_buff_size(struct device_node *np,
		struct aqo_device *aqo_dev)
{
	int rc;
	u32 val32;
	const char *key;

	key = "qcom,tx-buff-size";
	rc = of_property_read_u32(np, key, &val32);
	if (rc) {
		val32 = AQO_AQC_BUFF_SZ_DEFAULT;
		aqo_log(aqo_dev,
			"DT prop %s is missing for %s, using default %lu",
			key, np->name, val32);
	}

	aqo_dev->ch_tx.buff_size = val32;
	aqo_dev->ch_tx.buff_size =
		clamp_val(aqo_dev->ch_tx.buff_size,
			AQO_AQC_BUFF_SZ_MIN, AQO_AQC_BUFF_SZ_MAX);
	aqo_dev->ch_tx.buff_size =
		rounddown(aqo_dev->ch_tx.buff_size, AQO_AQC_BUFF_SZ_ALIGN);

	/* GSI receives buffer size as log2(buff_size) in a 16-bit field which
	 * implies the following theoretical limitations:
	 *   - Max buffer size of 2 ^ 65535 (which should never reach)
	 *   - Buffer size to be a power of 2
	 */
	aqo_dev->ch_tx.buff_size =
		roundup_pow_of_two(aqo_dev->ch_tx.buff_size);

	if (aqo_dev->ch_tx.buff_size > AQO_AQC_BUFF_SZ_MAX)
		aqo_dev->ch_tx.buff_size =
			rounddown_pow_of_two(aqo_dev->ch_tx.buff_size);

	if (aqo_dev->ch_tx.buff_size != val32)
		aqo_log(aqo_dev,
			"Tx buff size clamped/realigned to %u",
			aqo_dev->ch_tx.buff_size);

	return 0;
}

static int aqo_parse_tx_wrb_modc(struct device_node *np,
		struct aqo_device *aqo_dev)
{
	int rc;
	u32 val32;
	const char *key;

	/* Set invalid modc so that we will know when we set one below */
	aqo_dev->tx_wrb_mod_count = aqo_dev->ch_tx.ring_size + 1;

	key = "qcom,tx-wrb-mod-count";
	rc = of_property_read_u32(np, key, &val32);
	if (!rc) {
		aqo_dev->tx_wrb_mod_count = val32;
		aqo_dev->tx_wrb_mod_count =
			clamp_val(aqo_dev->tx_wrb_mod_count,
				AQO_GSI_TX_WRB_MODC_MIN,
				AQO_GSI_TX_WRB_MODC_MAX);
		aqo_dev->tx_wrb_mod_count =
			clamp_val(aqo_dev->tx_wrb_mod_count,
				0, aqo_dev->ch_tx.ring_size);
		if (aqo_dev->tx_wrb_mod_count != val32) {
			aqo_log(aqo_dev, "Tx WrB MODC clamped at %u",
				aqo_dev->tx_wrb_mod_count);
		}
	}

	key = "qcom,tx-wrb-mod-pc";
	rc = of_property_read_u32(np, key, &val32);
	if (!rc) {
		u32 pc = clamp_val(val32, 0, 100);
		u32 modc = (aqo_dev->ch_tx.ring_size * pc) / 100;

		if (pc != val32) {
			aqo_log(aqo_dev,
				"Tx WrB MODC PC clamped at %u percent", pc);
		}

		if (modc < aqo_dev->tx_wrb_mod_count) {
			aqo_log(aqo_dev,
				"Setting Tx WrB MODC to %u (%u %% of %u)",
				modc, pc, aqo_dev->ch_tx.ring_size);
			aqo_dev->tx_wrb_mod_count =
				clamp_val(modc,
					AQO_GSI_TX_WRB_MODC_MIN,
					AQO_GSI_TX_WRB_MODC_MAX);
			if (aqo_dev->tx_wrb_mod_count != modc) {
				aqo_log(aqo_dev, "Tx WrB MODC clamped at %u",
					aqo_dev->tx_wrb_mod_count);
			}
		}
	}

	/* Set default modc if a DT prop was not found above */
	if (aqo_dev->tx_wrb_mod_count > aqo_dev->ch_tx.ring_size) {
		aqo_dev->tx_wrb_mod_count = AQO_GSI_TX_WRB_MODC_DEFAULT;
		aqo_log(aqo_dev, "Setting Tx WrB MODC to default %u",
			aqo_dev->tx_wrb_mod_count);
	}

	return 0;
}

static int aqo_parse_tx_gsi_modc(struct device_node *np,
		struct aqo_device *aqo_dev)
{
	int rc;
	u32 val32;
	const char *key;

	/* Set invalid modc so that we will know when we set one below */
	aqo_dev->ch_tx.gsi_modc = aqo_dev->ch_tx.ring_size + 1;

	key = "qcom,tx-gsi-mod-count";
	rc = of_property_read_u32(np, key, &val32);
	if (!rc) {
		aqo_dev->ch_tx.gsi_modc = val32;
		aqo_dev->ch_tx.gsi_modc =
			clamp_val(aqo_dev->ch_tx.gsi_modc,
				AQO_GSI_MODC_MIN, AQO_GSI_MODC_MAX);
		aqo_dev->ch_tx.gsi_modc =
			clamp_val(aqo_dev->ch_tx.gsi_modc,
				0, aqo_dev->ch_tx.ring_size);
		if (aqo_dev->ch_tx.gsi_modc != val32) {
			aqo_log(aqo_dev, "Tx GSI MODC clamped at %u",
				aqo_dev->ch_tx.gsi_modc);
		}
	}

	key = "qcom,tx-gsi-mod-pc";
	rc = of_property_read_u32(np, key, &val32);
	if (!rc) {
		u32 pc = clamp_val(val32, 0, 100);
		u32 modc = (aqo_dev->ch_tx.ring_size * pc) / 100;

		if (pc != val32) {
			aqo_log(aqo_dev,
				"Tx GSI MODC PC clamped at %u percent", pc);
		}

		/* If both qcom,tx-gsi-mod-count and qcom,tx-gsi-mod-pc are
		 * specified, choose the lowest among them.
		 */
		if (modc < aqo_dev->ch_tx.gsi_modc) {
			aqo_log(aqo_dev,
				"Setting Tx GSI MODC to %u (%u %% of %u)",
				modc, pc, aqo_dev->ch_tx.ring_size);
			aqo_dev->ch_tx.gsi_modc =
				clamp_val(modc,
					AQO_GSI_MODC_MIN, AQO_GSI_MODC_MAX);
			if (aqo_dev->ch_tx.gsi_modc != modc) {
				aqo_log(aqo_dev, "Tx GSI MODC clamped at %u",
					aqo_dev->ch_tx.gsi_modc);
			}
		}
	}

	/* Set default modc if a DT prop was not found above */
	if (aqo_dev->ch_tx.gsi_modc > aqo_dev->ch_tx.ring_size) {
		aqo_dev->ch_tx.gsi_modc = AQO_GSI_DEFAULT_TX_MODC;
		aqo_log(aqo_dev, "Setting Tx GSI MODC to default %u",
			aqo_dev->ch_tx.gsi_modc);
	}

	return 0;
}

static int aqo_parse_tx_gsi_modt(struct device_node *np,
		struct aqo_device *aqo_dev)
{
	int rc;
	u32 val32;
	const char *key;

	key = "qcom,tx-gsi-mod-timer";
	rc = of_property_read_u32(np, key, &val32);
	if (!rc) {
		aqo_dev->ch_tx.gsi_modt = clamp_val(val32,
					AQO_GSI_MODT_MIN, AQO_GSI_MODT_MAX);
		if (aqo_dev->ch_tx.gsi_modt != val32) {
			aqo_log(aqo_dev, "Tx GSI MODT clamped at %u cycles",
				aqo_dev->ch_tx.gsi_modt);
		}
	} else {
		aqo_dev->ch_tx.gsi_modt = AQO_GSI_DEFAULT_TX_MODT;
		aqo_log(aqo_dev,
			"DT prop %s is missing for %s, using default %lu",
			key, np->name, aqo_dev->ch_tx.gsi_modt);
	}

	return 0;
}

static int aqo_parse_tx_props(struct device_node *np,
		struct aqo_device *aqo_dev)
{
	return aqo_parse_tx_ring_size(np, aqo_dev) ||
		aqo_parse_tx_buff_size(np, aqo_dev) ||
		aqo_parse_tx_wrb_modc(np, aqo_dev) ||
		aqo_parse_tx_gsi_modc(np, aqo_dev) ||
		aqo_parse_tx_gsi_modt(np, aqo_dev);
}

static int __aqo_parse_dt(struct aqo_device *aqo_dev)
{
	int rc;
	struct device_node *np = aqo_dev->eth_dev->dev->of_node;

	rc = aqo_parse_rx_proxies(np, aqo_dev);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to parse proxies");
		return rc;
	}

	rc = aqo_parse_rx_props(np, aqo_dev);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to parse rx properties");
		return rc;
	}

	rc = aqo_parse_tx_props(np, aqo_dev);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to parse tx properties");
		return rc;
	}

	aqo_dev->pci_direct = of_property_read_bool(np, "qcom,use-pci-direct");

	return rc;
}

static int aqo_parse_dt(struct aqo_device *aqo_dev)
{
	struct device *dev = aqo_dev->eth_dev->dev;

	if (!of_match_node(aquantia_of_matches, dev->of_node)) {
		aqo_log_err(aqo_dev, "Device tree node is not compatible");
		return -EINVAL;
	}

	aqo_dev->ch_rx.proxy.agent = AQO_PROXY_DEFAULT_AGENT;

	return __aqo_parse_dt(aqo_dev);
}

static int aqo_parse_pci(struct aqo_device *aqo_dev)
{
	struct device *dev = aqo_dev->eth_dev->dev;
	struct pci_dev *pci_dev = container_of(dev, struct pci_dev, dev);

	if (dev->bus != &pci_bus_type) {
		aqo_log(aqo_dev, "Device bus type is not PCI");
		return -EINVAL;
	}

	if (!pci_match_id(aquantia_pci_ids, pci_dev)) {
		aqo_log(aqo_dev, "Device PCI ID is not compatible");
		return -ENODEV;
	}

	aqo_dev->regs_base.paddr = pci_resource_start(pci_dev, 0);
	aqo_dev->regs_base.size = pci_resource_len(pci_dev, 0);

	aqo_log(aqo_dev, "PCI BAR 0 is at %p, size %zx",
		aqo_dev->regs_base.paddr, aqo_dev->regs_base.size);

	return 0;
}

static int aqo_parse(struct aqo_device *aqo_dev)
{
	int rc;

	rc = aqo_parse_pci(aqo_dev);
	if (!rc)
		rc = aqo_parse_dt(aqo_dev);

	return rc;
}

static int aqo_pair(struct ipa_eth_device *eth_dev)
{
	int rc = 0;
	struct aqo_device *aqo_dev;

	if (!eth_dev || !eth_dev->dev || !eth_dev->nd) {
		aqo_log_bug(NULL, "Invalid ethernet device structure");
		return -EFAULT;
	}

	aqo_dev = devm_kzalloc(eth_dev->dev, sizeof(*aqo_dev), GFP_KERNEL);
	if (!aqo_dev)
		return -ENOMEM;

	aqo_dev->eth_dev = eth_dev;

	aqo_log_dbg(aqo_dev, "Probing new device");

	rc = aqo_parse(aqo_dev);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to parse device");
		goto err_parse;
	}

	aqo_dev->regs_base.vaddr =
		ioremap_nocache(aqo_dev->regs_base.paddr,
			aqo_dev->regs_base.size);

	mutex_lock(&aqo_devices_lock);
	list_add(&aqo_dev->device_list, &aqo_devices);
	mutex_unlock(&aqo_devices_lock);

	eth_dev->od_priv = aqo_dev;

	aqo_log(aqo_dev, "Successfully probed new device");

	return 0;

err_parse:
	devm_kfree(eth_dev->dev, aqo_dev);
	return rc;
}

static void aqo_unpair(struct ipa_eth_device *eth_dev)
{
	struct device *dev = eth_dev->dev;
	struct aqo_device *aqo_dev = eth_dev->od_priv;

	aqo_log(aqo_dev, "Removing device");

	eth_dev->od_priv = NULL;

	mutex_lock(&aqo_devices_lock);
	list_del(&aqo_dev->device_list);
	mutex_unlock(&aqo_devices_lock);

	iounmap(aqo_dev->regs_base.vaddr);

	devm_kfree(dev, aqo_dev);
}

static int aqo_init_tx(struct ipa_eth_device *eth_dev)
{
	int rc = 0;
	struct aqo_device *aqo_dev = eth_dev->od_priv;

	rc = aqo_netdev_init_tx_channel(aqo_dev);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to initialize Tx AQC channel");
		goto err_netdev_init_ch;
	}

	rc = ipa_eth_ep_init(aqo_dev->ch_tx.eth_ch);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to initialize Tx IPA endpoint");
		goto err_ep_init;
	}

	rc = aqo_gsi_init_tx(aqo_dev);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to initialize Tx GSI rings");
		goto err_gsi_init;
	}

	rc = aqo_netdev_init_tx_event(aqo_dev);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to initialize Tx AQC event");
		goto err_netdev_init_ev;
	}

	aqo_log(aqo_dev, "Initialized Tx offload");

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
		aqo_log_err(aqo_dev, "Failed to start Tx AQC channel");
		goto err_netdev_start;
	}

	rc = aqo_gsi_start_tx(aqo_dev);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to start Tx GSI rings");
		goto err_gsi_start;
	}

	rc = ipa_eth_ep_start(aqo_dev->ch_tx.eth_ch);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to start Tx IPA endpoint");
		goto err_ep_start;
	}

	aqo_log(aqo_dev, "Started Tx offload");

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

	aqo_log(aqo_dev, "Stopped Tx offload");

	return 0;
}

static int aqo_deinit_tx(struct ipa_eth_device *eth_dev)
{
	struct aqo_device *aqo_dev = eth_dev->od_priv;

	// TODO: check return status
	aqo_netdev_deinit_tx_event(aqo_dev);
	aqo_gsi_deinit_tx(aqo_dev);
	aqo_netdev_deinit_tx_channel(aqo_dev);

	aqo_log(aqo_dev, "Deinitialized Tx offload");

	return 0;
}

static int aqo_init_rx(struct ipa_eth_device *eth_dev)
{
	int rc = 0;
	struct aqo_device *aqo_dev = eth_dev->od_priv;

	rc = aqo_netdev_init_rx_channel(aqo_dev);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to initialize Rx AQC channel");
		goto err_netdev_init_ch;
	}

	rc = ipa_eth_ep_init(aqo_dev->ch_rx.eth_ch);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to initialize Rx IPA endpoint");
		goto err_ep_init;
	}

	rc = aqo_gsi_init_rx(aqo_dev);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to initialize Rx GSI rings");
		goto err_gsi_init;
	}

	rc = aqo_proxy_init(aqo_dev);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to initialize Rx MSI proxy");
		goto err_proxy_init;
	}

	rc = aqo_netdev_init_rx_event(aqo_dev);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to initialize Rx AQC event");
		goto err_netdev_init_ev;
	}

	aqo_log(aqo_dev, "Initialized Rx offload");

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

	rc = ipa_eth_ep_start(aqo_dev->ch_rx.eth_ch);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to start Rx IPA endpoint");
		goto err_ep_start;
	}

	rc = aqo_gsi_start_rx(aqo_dev);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to start Rx GSI rings");
		goto err_gsi_start;
	}

	rc = aqo_proxy_start(aqo_dev);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to start Rx MSI proxy");
		goto err_proxy_start;
	}

	rc = aqo_netdev_start_rx(aqo_dev);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to start Rx AQC channel");
		goto err_netdev_start;
	}

	rc = aqo_netdev_rxflow_set(aqo_dev);
	if (rc) {
		aqo_log_err(aqo_dev, "Failed to set Rx flow to IPA");
		goto err_rxflow;
	}

	aqo_log(aqo_dev, "Started Rx offload");

	return 0;

err_rxflow:
	aqo_netdev_stop_rx(aqo_dev);
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
	aqo_netdev_rxflow_reset(aqo_dev);
	aqo_netdev_stop_rx(aqo_dev);
	aqo_proxy_stop(aqo_dev);
	aqo_gsi_stop_rx(aqo_dev);

	aqo_log(aqo_dev, "Stopped Rx offload");

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

	aqo_log(aqo_dev, "Deinitialized Rx offload");

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

#if IPA_ETH_API_VER >= 3
static int aqo_save_regs(struct ipa_eth_device *eth_dev,
		void **regs, size_t *size)
{
	size_t num_regs;
	struct aqo_device *aqo_dev = eth_dev->od_priv;
	struct aqo_regs *regs_save = &aqo_dev->regs_save;

	memset(regs_save, 0, sizeof(*regs_save));

	num_regs = aqo_regs_save(aqo_dev, regs_save);
	if (!num_regs)
		return -EFAULT;

	if (regs)
		*regs = regs_save;

	if (size)
		*size = sizeof(*regs_save);

	return 0;
}
#endif

static struct ipa_eth_offload_ops aqo_offload_ops = {
	.pair = aqo_pair,
	.unpair = aqo_unpair,

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

#if IPA_ETH_API_VER >= 3
	.save_regs = aqo_save_regs,
#endif
};

static struct ipa_eth_offload_driver aqo_offload_driver = {
	.name = aqo_driver_name,
	.bus = &pci_bus_type,
	.ops = &aqo_offload_ops,
};

int __init aqo_init(void)
{
	aqo_log(NULL, "Initializing AQC IPA offload driver");

	return ipa_eth_register_offload_driver(&aqo_offload_driver);
}
module_init(aqo_init);

void __exit aqo_exit(void)
{
	aqo_log(NULL, "Deinitializing AQC IPA offload driver");

	ipa_eth_unregister_offload_driver(&aqo_offload_driver);
}
module_exit(aqo_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("AQC IPA Offload Driver");
