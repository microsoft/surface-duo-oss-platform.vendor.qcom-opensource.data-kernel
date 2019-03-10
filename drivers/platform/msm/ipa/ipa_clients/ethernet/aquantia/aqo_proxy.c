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
	struct aqo_proxy_uc_context *uc_ctx = &aqo_dev->ch_rx.proxy.uc_ctx;

	uc_ctx->aqc_base = aqo_dev->regs_base.daddr;
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

	return aqo_uc_init_peripheral(uc_ctx->per_base);
}

static int proxy_start_uc(struct aqo_device *aqo_dev)
{
	struct aqo_proxy_uc_context *uc_ctx = &aqo_dev->ch_rx.proxy.uc_ctx;

	return aqo_uc_setup_channel(false, uc_ctx->aqc_ch, uc_ctx->gsi_ch);
}

static int proxy_stop_uc(struct aqo_device *aqo_dev)
{
	struct aqo_proxy_uc_context *uc_ctx = &aqo_dev->ch_rx.proxy.uc_ctx;

	return aqo_uc_teardown_channel(uc_ctx->gsi_ch);
}

static int proxy_deinit_uc(struct aqo_device *aqo_dev)
{
	return aqo_uc_deinit_peripheral();
}

/* Host/Linux as proxy agent */

static irqreturn_t proxy_rx_interrupt(int irq, void *data)
{
	dma_addr_t desc;
	u32 new_head;
	static u32 old_head;
	struct aqo_proxy_host_context *ctx = data;
	void *desc_vbase = ctx->desc_vbase;
	u16 max_head = ctx->desc_count - 1;

	new_head = readl_relaxed(ctx->aqc_hp) & (BIT(13) - 1);

#if 1
	//pr_crit("AQC: RECEIVED INTERRUPT head: %x, wp: %x\n", head, desc);

	while (new_head != old_head) {
		u32 last_desc = (new_head == 0) ? max_head : (new_head - 1);
		u32 *d = desc_vbase + (16 * last_desc);
		if ((d[2] & 0x1) == 0)
			new_head = last_desc;
		else
			break;
	}

	old_head = new_head;
#endif

	desc = ctx->desc_base + (16 * new_head);
	writel_relaxed((u32)desc, ctx->gsi_db);

	return IRQ_HANDLED;
}

static int proxy_init_host(struct aqo_device *aqo_dev)
{
	struct aqo_proxy_host_context *ctx = &aqo_dev->ch_rx.proxy.host_ctx;

	ctx->msi_addr.size = 4;
	ctx->msi_addr.daddr = ctx->msi_addr.paddr;

	ctx->aqc_base = aqo_dev->regs_base.vaddr;

	ctx->aqc_hp = AQC_RX_HEAD_PTR(ctx->aqc_base,
				AQO_ETHDEV(aqo_dev)->ch_rx->queue);
	ctx->gsi_db = ioremap_nocache(aqo_dev->ch_rx.gsi_db.paddr, 4);

	// FIXME: use dma addr?
	ctx->desc_base = AQO_ETHDEV(aqo_dev)->ch_rx->desc_mem.daddr;
	ctx->desc_vbase = AQO_ETHDEV(aqo_dev)->ch_rx->desc_mem.vaddr;
	ctx->desc_count = AQO_ETHDEV(aqo_dev)->ch_rx->desc_mem.size / 16;

	return 0;
}

static int proxy_start_host(struct aqo_device *aqo_dev)
{
	int rc;
	struct aqo_proxy_host_context *ctx = &aqo_dev->ch_rx.proxy.host_ctx;

	rc = request_irq(ctx->irq, proxy_rx_interrupt, IRQF_TRIGGER_RISING, "aqo-rx-irq", ctx);
	if (!rc) {
		pr_crit("AQC: IRQ REGISTERED\n");
	} else {
		pr_crit("AQC: FAILED IRQ REGISTER\n");
	}

	return rc;
}

static int proxy_stop_host(struct aqo_device *aqo_dev)
{
	struct aqo_proxy_host_context *ctx = &aqo_dev->ch_rx.proxy.host_ctx;

	free_irq(ctx->irq, ctx);

	return 0;
}

static int proxy_deinit_host(struct aqo_device *aqo_dev)
{
	struct aqo_proxy_host_context *ctx = &aqo_dev->ch_rx.proxy.host_ctx;

	iounmap(ctx->gsi_db);

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

	return -ENODEV;
}

int aqo_proxy_start(struct aqo_device *aqo_dev)
{
	enum aqo_proxy_agent agent = aqo_dev->ch_rx.proxy.agent;

	if (proxy_ops[agent].start)
		return proxy_ops[agent].start(aqo_dev);

	return -ENODEV;
}

int aqo_proxy_stop(struct aqo_device *aqo_dev)
{
	enum aqo_proxy_agent agent = aqo_dev->ch_rx.proxy.agent;

	if (proxy_ops[agent].stop)
		return proxy_ops[agent].stop(aqo_dev);

	return -ENODEV;
}

int aqo_proxy_deinit(struct aqo_device *aqo_dev)
{
	enum aqo_proxy_agent agent = aqo_dev->ch_rx.proxy.agent;

	if (proxy_ops[agent].deinit)
		return proxy_ops[agent].deinit(aqo_dev);

	return -ENODEV;
}
