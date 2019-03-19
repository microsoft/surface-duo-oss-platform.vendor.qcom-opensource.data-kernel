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

#include <linux/kthread.h>

#include "aqo_i.h"

/* uC as proxy agent */

static int proxy_init_uc(struct aqo_device *aqo_dev)
{
	int rc;
	struct aqo_proxy_uc_context *uc_ctx = &aqo_dev->ch_rx.proxy.uc_ctx;

	if (aqo_dev->pci_direct) {
		uc_ctx->aqc_base = aqo_dev->regs_base.paddr;
	} else {
		aqo_log_bug(aqo_dev, "Only PCI direct access is supported");
		return -EFAULT;
	}

	uc_ctx->aqc_ch = AQO_ETHDEV(aqo_dev)->ch_rx->queue;
	uc_ctx->gsi_ch = aqo_dev->ch_rx.gsi_ch;

	if (!uc_ctx->msi_addr.daddr) {
		uc_ctx->msi_addr.daddr = dma_map_resource(AQO_DEV(aqo_dev),
						uc_ctx->msi_addr.paddr, 0x4,
						DMA_FROM_DEVICE, 0);
	}

	uc_ctx->per_base = uc_ctx->aqc_base;

	if (aqo_dev->pci_direct)
		uc_ctx->per_base = AQO_PCI_DIRECT_SET(uc_ctx->per_base);

	rc = aqo_uc_init_peripheral(uc_ctx->per_base);
	if (rc)
		aqo_log_err(aqo_dev, "Failed to execute IPA uC Init command");
	else
		aqo_log(aqo_dev, "IPA uC Init command executed");

	return rc;
}

static int proxy_start_uc(struct aqo_device *aqo_dev)
{
	int rc;
	struct aqo_proxy_uc_context *uc_ctx = &aqo_dev->ch_rx.proxy.uc_ctx;

	rc = aqo_uc_setup_channel(false, uc_ctx->aqc_ch, uc_ctx->gsi_ch);
	if (rc)
		aqo_log_err(aqo_dev, "Failed to execute IPA uC Setup command");
	else
		aqo_log(aqo_dev, "IPA uC Setup command executed");

	return rc;
}

static int proxy_stop_uc(struct aqo_device *aqo_dev)
{
	int rc;
	struct aqo_proxy_uc_context *uc_ctx = &aqo_dev->ch_rx.proxy.uc_ctx;

	rc = aqo_uc_teardown_channel(uc_ctx->gsi_ch);
	if (rc)
		aqo_log_err(aqo_dev,
			"Failed to execute IPA uC Teardown command");
	else
		aqo_log(aqo_dev, "IPA uC Teardown command executed");

	return rc;
}

static int proxy_deinit_uc(struct aqo_device *aqo_dev)
{
	int rc = aqo_uc_deinit_peripheral();
	if (rc)
		aqo_log_err(aqo_dev, "Failed to execute IPA uC Deinit command");
	else
		aqo_log(aqo_dev, "IPA uC Denit command executed");

	return rc;
}

/* Host/Linux as proxy agent */

static inline bool aqc_desc_dd(const u32 *desc_words)
{
	return (desc_words[2] & 0x1);
}

static irqreturn_t proxy_rx_interrupt(int irq, void *data)
{
	struct aqo_proxy_host_context *host_ctx = data;

	const u32 old_head = host_ctx->head;
	const u32 max_head = host_ctx->max_head;
	const void *desc_vbase = host_ctx->desc_vbase;

	u32 head = readl_relaxed(host_ctx->aqc_hp) & (BIT(13) - 1);

	/* AQC MAC moves head pointer once the descriptor write is queued to
	 * AQC PCI Host Interface (PHI). At the time we read head pointer, it
	 * is possible that the PHI has yet to commit last descriptor to DDR.
	 * Reverse iterate through descriptors until we find a valid Rx WrB
	 * descriptor (Descritor Done bit == 1) and thereby head pointer.
	 */
	while (head != old_head) {
		const u32 phead = (head == 0) ? max_head : (head - 1);
		const u32 *last_desc = desc_vbase + (16 * phead);

		if (likely(aqc_desc_dd(last_desc)))
			break;

		head = phead;
	}

	host_ctx->head = head;

	writel_relaxed((u32)(host_ctx->desc_dbase + (16 * head)),
		host_ctx->gsi_db);

	return IRQ_HANDLED;
}

static int proxy_init_host(struct aqo_device *aqo_dev)
{
	struct aqo_proxy_host_context *host_ctx =
		&aqo_dev->ch_rx.proxy.host_ctx;

	host_ctx->msi_addr.size = 4;
	host_ctx->msi_addr.daddr = dma_map_resource(AQO_DEV(aqo_dev),
						host_ctx->msi_addr.paddr,
						host_ctx->msi_addr.size,
						DMA_FROM_DEVICE, 0);

	host_ctx->aqc_base = aqo_dev->regs_base.vaddr;

	host_ctx->aqc_hp = AQC_RX_HEAD_PTR(host_ctx->aqc_base,
				AQO_ETHDEV(aqo_dev)->ch_rx->queue);
	host_ctx->gsi_db = ioremap_nocache(aqo_dev->ch_rx.gsi_db.paddr, 4);

	aqo_log_dbg(aqo_dev, "Mapped GSI DB at %pa to %px",
			aqo_dev->ch_rx.gsi_db.paddr, host_ctx->gsi_db);

	host_ctx->desc_dbase = AQO_ETHDEV(aqo_dev)->ch_rx->desc_mem.daddr;
	host_ctx->desc_vbase = AQO_ETHDEV(aqo_dev)->ch_rx->desc_mem.vaddr;
	host_ctx->max_head = AQO_ETHDEV(aqo_dev)->ch_rx->desc_count - 1;

	aqo_log(aqo_dev, "Initialized Rx MSI host proxy");

	return 0;
}

static int proxy_start_host(struct aqo_device *aqo_dev)
{
	int rc;
	struct aqo_proxy_host_context *host_ctx =
		&aqo_dev->ch_rx.proxy.host_ctx;

	rc = request_irq(host_ctx->irq, proxy_rx_interrupt,
				IRQF_TRIGGER_RISING, "aqo-irq", host_ctx);
	if (rc)
		aqo_log_err(aqo_dev, "Failed to register interrupt handler");
	else
		aqo_log(aqo_dev, "Registered interrupt handler");

	aqo_log(aqo_dev, "Started Rx MSI host proxy");

	return rc;
}

static int proxy_stop_host(struct aqo_device *aqo_dev)
{
	struct aqo_proxy_host_context *host_ctx =
		&aqo_dev->ch_rx.proxy.host_ctx;

	free_irq(host_ctx->irq, host_ctx);

	aqo_log(aqo_dev, "Stopepd Rx MSI host proxy");

	return 0;
}

static int proxy_deinit_host(struct aqo_device *aqo_dev)
{
	struct aqo_proxy_host_context *host_ctx =
		&aqo_dev->ch_rx.proxy.host_ctx;

	iounmap(host_ctx->gsi_db);
	host_ctx->gsi_db = 0;

	dma_unmap_resource(AQO_DEV(aqo_dev),
		host_ctx->msi_addr.daddr, host_ctx->msi_addr.size,
		DMA_FROM_DEVICE, 0);
	host_ctx->msi_addr.daddr = 0;

	aqo_log(aqo_dev, "Deinitialized AQC Rx MSI host proxy");

	return 0;
}

struct
{
	int (*init)(struct aqo_device *aqo_dev);
	int (*start)(struct aqo_device *aqo_dev);
	int (*stop)(struct aqo_device *aqo_dev);
	int (*deinit)(struct aqo_device *aqo_dev);

} proxy_ops[AQO_PROXY_MAX_AGENTS] = {
	[AQO_PROXY_UC] = {
		.init = proxy_init_uc,
		.start = proxy_start_uc,
		.stop = proxy_stop_uc,
		.deinit = proxy_deinit_uc,
	},
	[AQO_PROXY_HOST] = {
		.init = proxy_init_host,
		.start = proxy_start_host,
		.stop = proxy_stop_host,
		.deinit = proxy_deinit_host,
	},
};

int aqo_proxy_init(struct aqo_device *aqo_dev)
{
	enum aqo_proxy_agent agent = aqo_dev->ch_rx.proxy.agent;

	if (proxy_ops[agent].init)
		return proxy_ops[agent].init(aqo_dev);

	aqo_log_err(aqo_dev,
		"Init operation not supported by proxy agent %u", agent);

	return -ENODEV;
}

int aqo_proxy_start(struct aqo_device *aqo_dev)
{
	enum aqo_proxy_agent agent = aqo_dev->ch_rx.proxy.agent;

	if (proxy_ops[agent].start)
		return proxy_ops[agent].start(aqo_dev);

	aqo_log_err(aqo_dev,
		"Start operation not supported by proxy agent %u", agent);

	return -ENODEV;
}

int aqo_proxy_stop(struct aqo_device *aqo_dev)
{
	enum aqo_proxy_agent agent = aqo_dev->ch_rx.proxy.agent;

	if (proxy_ops[agent].stop)
		return proxy_ops[agent].stop(aqo_dev);

	aqo_log_err(aqo_dev,
		"Stop operation not supported by proxy agent %u", agent);

	return -ENODEV;
}

int aqo_proxy_deinit(struct aqo_device *aqo_dev)
{
	enum aqo_proxy_agent agent = aqo_dev->ch_rx.proxy.agent;

	if (proxy_ops[agent].deinit)
		return proxy_ops[agent].deinit(aqo_dev);

	aqo_log_err(aqo_dev,
		"Deinit operation not supported by proxy agent %u", agent);

	return -ENODEV;
}
