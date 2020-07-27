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

#include <eth-adaption-main.h>
#include <eth-adaption-server.h>
#include <eth-adaption-client.h>
#include <soc/qcom/qrtr_ethernet.h>

/* insmod parameters, currently hard coded. */
int server = 1;
module_param(server, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(server, "Ethernet Adaptation Layer mode  [0-Client, 1-Server]");

char *vlan_intf = "eth0.124";
module_param(vlan_intf, charp, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(vlan_intf, "VLAN interface [eth0.124]");

int connect_retry_cnt = 5;
module_param(connect_retry_cnt, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(connect_retry_cnt, "Client retry count [5]");

int iptype= 0;
module_param(iptype, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(iptype, "Dest IP address type[0-IPV4, 1-IPV6]");

char *destipv6 = "fd53:7cb8:383:7c::2";
module_param(destipv6, charp, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(destipv6, "Destination IPV6 address [fd53:7cb8:383:7c::2]");

char *destipv4 = "192.168.1.5";
module_param(destipv4, charp, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(destipv4, "Destination IPV4 address [10.129.41.200]");

int dest_port=5020;
module_param(dest_port, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(iptype, "Destination TCP port[5020]");


struct eth_adapt_device
{
	struct device dev;
	void *user_data;
};

struct eth_adapt_device eth_dev;
struct eth_adapt_result eth_res;

bool qrtr_init;

/**
* eth_adaption_notifier_device_event()handler function for link up and down evts.
*
* @notifier_block:
* @event:
* @ptr:
* Return:int
*/
static int eth_adaption_notifier_device_event
(
	struct notifier_block *unused,
	unsigned long event,
	void *ptr
)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	if(dev)
	{
		if(strcmp(vlan_intf, dev->name) != 0)
			return NOTIFY_DONE;
		ETHADPTDBG("eth_adaption_notifier_device_event dev name %s ,%d, %d, %d\n",dev->name,event,dev->operstate,qrtr_init);

		if(qrtr_init == false)
			return NOTIFY_DONE;
		switch (event) {
		case NETDEV_DOWN:
		case NETDEV_UP:
			qcom_ethernet_qrtr_status_cb(event);
			break;
		case NETDEV_CHANGE:
			// query dev_link
			if(dev->operstate == IF_OPER_LOWERLAYERDOWN) {
				qcom_ethernet_qrtr_status_cb(NETDEV_DOWN);
				eth_adaption_notifier_soft_reset();
				}
			else if (dev->operstate == IF_OPER_UP) {
				qcom_ethernet_qrtr_status_cb(NETDEV_UP);
			}
			break;
		}
	}
done:
	return NOTIFY_DONE;
}

static struct notifier_block eth_adaption_notifier = {
	.notifier_call = eth_adaption_notifier_device_event,
};

/**
* eth_adapt_send() - Function to send QMI packet from IPCRTR over TCP socket.
*
* @skb: buffer holding QMI message.
*
* Use this API from IPCRTR ethernet transport layer.
*
* Return: 0 on success, non-zero otherwise
*/
int eth_adapt_send(struct sk_buff *skb)
{
	int ret = 0;
	if (skb == NULL)
	return -1;

	if (server)
	{
		ret = server_send(skb->data,skb->len);
	}
	else
	{
		ret = client_send(skb->data,skb->len);
	}
	return 0;
}
EXPORT_SYMBOL(eth_adapt_send);

/**
* eth_adapt_register_netdevice_notifier() - register for link up and down evts.
*
* @void:
*
* Return:void
*/
static inline void eth_adapt_register_netdevice_notifier(void)
{
	register_netdevice_notifier(&eth_adaption_notifier);
}

/**
* eth_adapt_unregister_netdevice_notifier() - unregister for link up and down evts.
*
* @void:
*
* Return:void
*/
static inline void eth_adapt_unregister_netdevice_notifier(void)
{
	unregister_netdevice_notifier(&eth_adaption_notifier);
}

/**
* eth_adapt_init() - Initialize Ethernet adaptation module.
* Wait till file system comes up.
* Read .ini file for destination mac address, vlan id.
*
* Return: 0 on success, non-zero otherwise
*/
static int __init eth_adapt_init(void)
{
	int ret=0;

	ETHADPTDBG("eth_adapt_init\n");
	ETHADPTDBG("eth_adapt_init destipv4 %s\n",destipv4);
	ETHADPTDBG("eth_adapt_init destipv6 %s\n",destipv6);
	ETHADPTDBG("eth_adapt_init dest_port %d\n",dest_port);
	ETHADPTDBG("eth_adapt_init iptype %d\n",iptype);
	ETHADPTDBG("eth_adapt_init server %d\n",server);
// register for eth netdev linkup linkdown events.
	if(server)
	{
	// start server
		ret=server_init(dest_port,iptype,connect_retry_cnt);
	}
	else
	{
		if (iptype == 0)
		{
			ret = client_connect(destipv4,iptype,dest_port,connect_retry_cnt);
		}
		else
		{
			ret = client_connect(destipv6,iptype,dest_port,connect_retry_cnt);
		}
	}
	eth_adapt_register_netdevice_notifier();
#ifdef CONFIG_MSM_BOOT_TIME_MARKER
		place_marker("M - eth-adaption-layer init");
#endif
	return ret;
}

/**
* eth_adapt_exit() - Initialize Ethernet adaptation module.
* Exit Ethernet adaptation module.
* Read .ini file for destination mac address, vlan id.
*
* Return: 0 on success, non-zero otherwise
*/
static void __exit eth_adapt_exit(void)
{
	ETHADPTDBG("eth_adapt_exit\n");

	if(qrtr_init)
		qcom_ethernet_qrtr_status_cb(NETDEV_DOWN);

	eth_adapt_unregister_netdevice_notifier();

	if(server)
		server_cleanup();
	else
		client_cleanup();

#ifdef CONFIG_MSM_BOOT_TIME_MARKER
	place_marker("M - eth-adaption-layer eth_adapt_exit");
#endif
}

/**
* eth_adaption_notifier_soft_reset() - resets Ethernet adaptation module.
* Return: void
*/
void eth_adaption_notifier_soft_reset(void)
{
	ETHADPTDBG("eth_adaption_notifier_soft_reset entry \n");
	if(server)
	{
		server_cleanup();
		ETHADPTDBG("eth_adaption_notifier_soft_reset server init \n");
		server_init(dest_port,iptype,connect_retry_cnt);
	}
	else
	{
		client_cleanup();
		if (iptype == 0)
		{
			client_connect(destipv4,iptype,dest_port,connect_retry_cnt);
		}
		else
		{
			client_connect(destipv6,iptype,dest_port,connect_retry_cnt);
		}
	}
}

module_init(eth_adapt_init)
module_exit(eth_adapt_exit)

MODULE_AUTHOR("Abhishek B Chauhan -Qualcomm inc");
MODULE_DESCRIPTION("QTI Ethernet Adaptation Module");
MODULE_LICENSE("GPL v2");
