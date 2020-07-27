/* Copyright (c) 2020, The Linux Foundation. All rights reserved.

* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 and
* only version 2 as published by the Free Software Foundation.

* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*/

/*
 * Ethernet adaptation module to interface with IPCRTR.
 * Owner - Abhishek B Chauhan - 8/18/2020
*/

#include<linux/init.h>
#include<linux/module.h>
#include<linux/kernel.h>
#include<linux/socket.h>
#include<linux/in.h>
#include<linux/in6.h>
#include<linux/net.h>
#include<linux/tcp.h>
#include<linux/slab.h>
#include<linux/inet.h>
#include<linux/syscalls.h>
#include<linux/netdev_features.h>
#include<linux/netdevice.h>
#include<linux/types.h>
#include<linux/platform_device.h>
#include<linux/phy.h>


#include<net/protocol.h>
#include<net/addrconf.h>
#include<net/sock.h>

#include<net/inet_common.h>
#include<net/tcp_states.h>
#include<net/tcp.h>

#include<asm/uaccess.h>

#ifdef CONFIG_MSM_BOOT_TIME_MARKER
#include <soc/qcom/boot_stats.h>
#endif

#define DRV_NAME "eth-adaption-layer"
#define MAX_SIZE 8192
#define ETHADPTDBG(fmt, args...) \
do {\
	pr_debug(DRV_NAME " %s:%d " fmt, __func__, __LINE__, ## args);\
} while (0)
#define ETHADPTERR(fmt, args...) \
do {\
	pr_err(DRV_NAME " %s:%d " fmt, __func__, __LINE__, ## args);\
} while (0)
#define ETHADPTINFO(fmt, args...) \
do {\
	pr_info(DRV_NAME " %s:%d " fmt, __func__, __LINE__, ## args);\
} while (0)

/**
* eth_adapt_send() - Function to send QMI packet from IPCRTR over TCP socket.
*
* @skb: buffer holding QMI message.
*
* Use this API from IPCRTR ethernet transport layer.
*
* Return: 0 on success, non-zero otherwise
*/
int eth_adapt_send(struct sk_buff *skb);

/**
* eth_adaption_notifier_soft_reset() - resets Ethernet adaptation module.
* Return: void
*/
void eth_adaption_notifier_soft_reset(void);
