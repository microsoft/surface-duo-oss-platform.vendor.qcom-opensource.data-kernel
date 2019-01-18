/* Copyright (c) 2019, The Linux Foundation. All rights reserved.
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
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/hashtable.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <nss_api_if.h>

#include <../net/rmnet_data/rmnet_nss.h>

#define RMNET_NSS_HASH_BITS 8

#define hash_add_ptr(table, node, key) \
	hlist_add_head(node, &table[hash_ptr(key, HASH_BITS(table))])

static DEFINE_HASHTABLE(rmnet_nss_ctx_hashtable, RMNET_NSS_HASH_BITS);

struct rmnet_nss_ctx {
	struct hlist_node hnode;
	struct net_device *rmnet_dev;
	struct nss_virt_if_handle *nss_ctx;
};

static struct rmnet_nss_ctx *rmnet_nss_find_ctx(struct net_device *dev)
{
	struct rmnet_nss_ctx *ctx;
	struct hlist_head *bucket;
	u32 hash;

	hash = hash_ptr(dev, HASH_BITS(rmnet_nss_ctx_hashtable));
	bucket = &rmnet_nss_ctx_hashtable[hash];
	hlist_for_each_entry(ctx, bucket, hnode) {
		if (ctx->rmnet_dev == dev)
			return ctx;
	}

	return NULL;
}

static void rmnet_nss_free_ctx(struct rmnet_nss_ctx *ctx)
{
	if (ctx) {
		hash_del(&ctx->hnode);
		nss_virt_if_unregister(ctx->nss_ctx);
		nss_virt_if_destroy_sync(ctx->nss_ctx);
		kfree(ctx);
	}
}

/* Pull off an ethernet header. Used for DL exception and UL cases */
int rmnet_nss_rx(struct sk_buff *skb)
{
	if (!skb->protocol || skb->protocol == htons(ETH_P_802_3))
		return !skb_pull(skb, sizeof(struct ethhdr));
	else
		return -1;
}

/* Downlink
 *
 * Push ethernet header
 * - src and dst = 00:00:00:00:00:00
 * - protocol = ETH_P_IP or ETH_P_IPV6
 * set skb procol tp ETH_P_802_3
 * IP header must be 4 byte aligned, MAC will not be aligned
 * nss_virt_if_tx_buf(ctx, skb)
 */
int rmnet_nss_tx(struct sk_buff *skb)
{
	struct ethhdr *eth;
	struct rmnet_nss_ctx *ctx;
	struct net_device *dev = skb->dev;
	nss_tx_status_t rc;
	unsigned int len;
	u8 version = ((struct iphdr *)skb->data)->version;

	ctx = rmnet_nss_find_ctx(dev);
	if (!ctx)
		return -EINVAL;

	eth = (struct ethhdr *)skb_push(skb, sizeof(*eth));
	memset(&eth->h_dest, 0, ETH_ALEN * 2);
	eth->h_proto = (version == 4) ? htons(ETH_P_IP) : htons(ETH_P_IPV6);
	skb->protocol = htons(ETH_P_802_3);
	/* Get length including ethhdr */
	len = skb->len;

	rc = nss_virt_if_tx_buf(ctx->nss_ctx, skb);
	if (rc == NSS_TX_SUCCESS) {
		/* Increment rmnet_data device stats.
		 * Don't call rmnet_data_vnd_rx_fixup() to do this, as
		 * there's no guarantee the skb pointer is still valid.
		 */
		dev->stats.rx_packets++;
		dev->stats.rx_bytes += len;
		return 0;
	}

	return 1;
}

/* Called by NSS in the DL exception case.
 * Since the packet cannot be sent over the accelerated path, we need to
 * handle it. Remove the ethernet header and pass it onward to the stack
 * if possible.
 */
void rmnet_nss_receive(struct net_device *dev, struct sk_buff *skb,
		       struct napi_struct *napi)
{
	if (!skb)
		return;

	if (rmnet_nss_rx(skb))
		goto drop;

	/* reset header pointers */
	skb_reset_transport_header(skb);
	skb_reset_network_header(skb);
	skb_reset_mac_header(skb);

	/* reset packet type */
	skb->pkt_type = PACKET_HOST;

	/* reset protocol type */
	switch (skb->data[0] & 0xF0) {
	case 0x40:
		skb->protocol = htons(ETH_P_IP);
		break;
	case 0x60:
		skb->protocol = htons(ETH_P_IPV6);
		break;
	default:
		goto drop;
	}

	netif_receive_skb(skb);
	return;

drop:
	kfree_skb(skb);
}

/* Create and register an NSS context for an rmnet_data device */
int rmnet_nss_create_vnd(struct net_device *dev)
{
	struct rmnet_nss_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_ATOMIC);
	if (!ctx)
		return -ENOMEM;

	ctx->rmnet_dev = dev;
	ctx->nss_ctx = nss_virt_if_create_sync(dev);
	if (!ctx->nss_ctx) {
		kfree(ctx);
		return -1;
	}

	nss_virt_if_register(ctx->nss_ctx, rmnet_nss_receive, dev);
	hash_add_ptr(rmnet_nss_ctx_hashtable, &ctx->hnode, dev);
	return 0;
}

/* Unregister and destroy the NSS context for an rmnet_data device */
int rmnet_nss_free_vnd(struct net_device *dev)
{
	struct rmnet_nss_ctx *ctx;

	ctx = rmnet_nss_find_ctx(dev);
	rmnet_nss_free_ctx(ctx);

	return 0;
}

static const struct rmnet_nss_cb rmnet_nss = {
	.nss_create = rmnet_nss_create_vnd,
	.nss_free = rmnet_nss_free_vnd,
	.nss_tx = rmnet_nss_tx,
	.nss_rx = rmnet_nss_rx,
};

int __init rmnet_nss_init(void)
{
	pr_err("%s(): initializing rmnet_nss\n", __func__);
	RCU_INIT_POINTER(rmnet_nss_callbacks, &rmnet_nss);
	return 0;
}

void __exit rmnet_nss_exit(void)
{
	struct hlist_node *tmp;
	struct rmnet_nss_ctx *ctx;
	int bkt;

	pr_err("%s(): exiting rmnet_nss\n", __func__);
	RCU_INIT_POINTER(rmnet_nss_callbacks, NULL);

	/* Tear down all NSS contexts */
	hash_for_each_safe(rmnet_nss_ctx_hashtable, bkt, tmp, ctx, hnode)
		rmnet_nss_free_ctx(ctx);
}

MODULE_LICENSE("GPL v2");
module_init(rmnet_nss_init);
module_exit(rmnet_nss_exit);
