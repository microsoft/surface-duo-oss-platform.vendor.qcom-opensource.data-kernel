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
#include <soc/qcom/sb_notification.h>

/* Mutex lock */
struct mutex eam_lock;
/* debug fs directory for stats*/
struct dentry *debugfs_dir;

/* global data for stats*/
unsigned long receive_allocfree_stat;
unsigned long send_data;
unsigned long recevied_data;
unsigned long error_stat;

/*Kthread global structure to receive SSR IRQ context and handle it*/
struct kthread_worker sb_kworker;
struct task_struct *sb_task;
struct kthread_work sb_link_up;
struct kthread_work sb_link_down;

struct eth_adapt_device
{
	struct device dev;
	void *user_data;
};

struct eth_adapt_device eth_dev;
struct eth_adapt_result eth_res;

/* Critical section variable for QRTR initialization */
int qrtr_init;

/* Critical section variable for QRTR initialization */
int link_state;

/* SSR notifier block for SSR event handling */
struct notifier_block qrtr_nb;

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

/**
* eth_adaption_set_link_state()handler function set link up and down evts.
* @event:
* @ptr:
* Return:void
*/
static void eth_adaption_set_link_state(int event)
{
	/* Critical section */
	mutex_lock(&eam_lock);
	link_state = event;
	mutex_unlock(&eam_lock);
}


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
		ETHADPTINFO("eth_adaption_notifier_device_event %d, %d, %d\n",event,qrtr_init,link_state);
		switch (event) {
		case NETDEV_DOWN:
			if(qrtr_init == QRTR_DEINIT || qrtr_init == QRTR_INPROGRESS || qrtr_init == QRTR_CONNFAILED)
				return NOTIFY_DONE;
			eth_adaption_set_link_state(NETDEV_DOWN);
			qcom_ethernet_qrtr_status_cb(NETDEV_DOWN);
			break;
		case NETDEV_UP:
			eth_adaption_set_link_state(NETDEV_UP);
			if(qrtr_init == QRTR_INIT)
			{
				qcom_ethernet_qrtr_status_cb(NETDEV_UP);
			}
			else if(qrtr_init == QRTR_INPROGRESS)
			{
				ETHADPTINFO("Wait until connection retry ends.");
			}
			else if(qrtr_init == QRTR_DEINIT)
			{
				kthread_queue_work(&sb_kworker, &sb_link_up);
			}
			else if(qrtr_init == QRTR_CONNFAILED)
			{
				kthread_queue_work(&sb_kworker, &sb_link_down);
				kthread_queue_work(&sb_kworker, &sb_link_up);
			}
			break;
		case NETDEV_CHANGE:
			// query dev_link
			if(dev->operstate == IF_OPER_LOWERLAYERDOWN) {
				kthread_queue_work(&sb_kworker, &sb_link_down);
				}
			else if (dev->operstate == IF_OPER_UP) {
				if(qrtr_init == QRTR_CONNFAILED)
					kthread_queue_work(&sb_kworker, &sb_link_down);
				kthread_queue_work(&sb_kworker, &sb_link_up);
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
* eth_adaption_sb_notifier_device_event()handler function for SSR events
*
* @notifier_block:
* @event:
* @ptr:
* Return:int
*/
static int eth_adaption_sb_notifier_device_event
(
	struct notifier_block *unused,
	unsigned long event,
	void *ptr
)
{
	switch(event)
	{
		case EVENT_REMOTE_STATUS_DOWN:
			ETHADPTINFO("eth_adaption_sb_notifier_device_event %d, %d, %d\n",event,qrtr_init,link_state);
			kthread_queue_work(&sb_kworker, &sb_link_down);
			break;
		case EVENT_REMOTE_STATUS_UP:
			ETHADPTINFO("eth_adaption_sb_notifier_device_event %d, %d, %d\n",event,qrtr_init,link_state);
			if(qrtr_init == QRTR_CONNFAILED)
				kthread_queue_work(&sb_kworker, &sb_link_down);
			kthread_queue_work(&sb_kworker, &sb_link_up);
			break;
	}
	return NOTIFY_DONE;
}

/**
* eth_adaption_init_notifier_thread()handler function to init notifier kthread
* @void
* Return:void
*/
static void eth_adaption_init_notifier_thread(void)
{
	kthread_init_work(&sb_link_up, eth_adaption_notifier_soft_set);
	kthread_init_work(&sb_link_down, eth_adaption_notifier_soft_reset);
	kthread_init_worker(&sb_kworker);
	sb_task = kthread_run(kthread_worker_fn, &sb_kworker, "eth_adapt_notifier");

	if (IS_ERR(sb_task))
	{
		ETHADPTERR("%s: Error allocating wq\n", __func__);
		return;
	}
}

/**
* eth_adaption_sb_register_listener()handler function to register for SSR events
* @void
* Return:int
*/
static void eth_adaption_sb_register_listener(void)
{
	int ret;
	qrtr_nb.notifier_call = eth_adaption_sb_notifier_device_event;
	ret = sb_register_evt_listener(&qrtr_nb);
	if (ret)
	{
		ETHADPTERR("failed at: %s\n", __func__);
		return;
	}

}


/**
* eth_adaption_sb_unregister_listener()handler function to un register for SSR events
* @void
* Return:int
*/
static void eth_adaption_sb_unregister_listener(void)
{
	int ret;
	ret =  sb_unregister_evt_listener(&qrtr_nb);
	if (ret)
	{
		ETHADPTERR("failed at: %s\n", __func__);
		return;
	}
	if(sb_task)
	{
		kthread_cancel_work_sync(&sb_link_up);
		kthread_cancel_work_sync(&sb_link_down);
		kthread_flush_work(&sb_link_up);
		kthread_flush_work(&sb_link_down);
		kthread_flush_worker(&sb_kworker);
		kthread_stop(sb_task);
		sb_task = NULL;
	}
}


/**
* eth_adaption_current_stats()handler function for file operations.
* @file:
* @user_buf:
* @count:
* @ppos:
* Return:ssize_t
*/
static ssize_t eth_adaption_current_stats(struct file *file,
					 const char __user *user_buf,
					  size_t count, loff_t *ppos)
{
	s8 option = 0;
	char in_buf[2];
	unsigned long ret;

	if (sizeof(in_buf) < 2)
		return -EFAULT;

	ret = copy_from_user(in_buf, user_buf, 1);
	if (ret)
		return -EFAULT;

	in_buf[1] = '\0';
	if (kstrtos8(in_buf, 0, &option))
		return -EFAULT;

	if (option == 1)
	{
		ETHADPTINFO("destipv4 %s\n",destipv4);
		ETHADPTINFO("destipv6 %s\n",destipv6);
		ETHADPTINFO("dest_port %d\n",dest_port);
		ETHADPTINFO("iptype %d\n",iptype);
		ETHADPTINFO("server (1) or client (0) %d\n",server);
		ETHADPTINFO("retry count %d\n",connect_retry_cnt);
		ETHADPTINFO("vlan sinterface %s\n",vlan_intf);
		ETHADPTINFO("Received data in byte :%ld\n",recevied_data);
		ETHADPTINFO("Sent data in byte :%ld\n",send_data );
		ETHADPTINFO("Error data in byte :%ld\n",error_stat );
		ETHADPTINFO("memstat 0 or 1 else memleak:%ld\n", receive_allocfree_stat);
	}
	else
	{
		ETHADPTERR("Operation not permitted\n");
	}

	return count;
}

/**
* debugfs eth_adaption_dump for file operations.
*/
static const struct file_operations eth_adaption_dump = {
	.write = eth_adaption_current_stats,
	.open = simple_open,
	.owner = THIS_MODULE,
	.llseek = default_llseek,
};

/**
* eth_adaption_create_debugfs() - Function to create debug fs.
*
* @void:.
*
* Use this API from main thread.
*
* Return: 0 on success, non-zero otherwise
*/
static int eth_adaption_create_debugfs(void)
{
	static struct dentry *eth_adapt_dump;

	debugfs_dir = debugfs_create_dir("eth-adapt", NULL);

	if (!debugfs_dir || IS_ERR(debugfs_dir)) {
		ETHADPTERR("Can't create debugfs dir\n");
		return -ENOMEM;
	}

	eth_adapt_dump = debugfs_create_file("tx_rx_data_dump", 0400,
					   debugfs_dir, NULL,
					   &eth_adaption_dump);
	if (!eth_adapt_dump || IS_ERR(eth_adapt_dump)) {
		ETHADPTERR("Can't create eth_adapt_dump %d\n", (int)eth_adapt_dump);
		goto fail;
	}

	return 0;

fail:
	debugfs_remove_recursive(debugfs_dir);
	return -ENOMEM;
}


/**
* eth_adaption_send() - Function to send QMI packet from IPCRTR over TCP socket.
*
* @skb: buffer holding QMI message.
*
* Use this API from IPCRTR ethernet transport layer.
*
* Return: 0 on success, non-zero otherwise
*/
int eth_adaption_send(struct sk_buff *skb)
{
	int ret = 0;
	if (skb == NULL)
	return -1;

	if (server)
	{
		ret = eth_adaption_server_send(skb->data,skb->len);
	}
	else
	{
		ret = eth_adaption_client_send(skb->data,skb->len);
	}
	return 0;
}
EXPORT_SYMBOL(eth_adaption_send);

/**
* eth_adapt_register_netdevice_notifier() - register for link up and down evts.
*
* @void:
*
* Return:void
*/
static inline void eth_adaption_register_netdevice_notifier(void)
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
static inline void eth_adaption_unregister_netdevice_notifier(void)
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
static int __init eth_adaption_init(void)
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
		ret=eth_adaption_server_connect(dest_port,iptype,connect_retry_cnt);
	}
	else
	{
		if (iptype == 0)
		{
			ret = eth_adaption_client_connect(destipv4,iptype,dest_port,connect_retry_cnt);
		}
		else
		{
			ret = eth_adaption_client_connect(destipv6,iptype,dest_port,connect_retry_cnt);
		}
	}
	eth_adaption_init_notifier_thread();
	eth_adaption_register_netdevice_notifier();
	eth_adaption_sb_register_listener();
	eth_adaption_create_debugfs();
#ifdef CONFIG_MSM_BOOT_TIME_MARKER
		place_marker("M - eth-adaption-layer init");
#endif
	return ret;
}

/**
* eth_adaption_exit() - Initialize Ethernet adaptation module.
* Exit Ethernet adaptation module.
* Read .ini file for destination mac address, vlan id.
*
* Return: 0 on success, non-zero otherwise
*/
static void __exit eth_adaption_exit(void)
{
	ETHADPTDBG("eth_adapt_exit\n");

	if(qrtr_init == QRTR_INIT && link_state != NETDEV_DOWN)
		qcom_ethernet_qrtr_status_cb(NETDEV_DOWN);

	/* Critical section */
	mutex_lock(&eam_lock);
	qrtr_init = QRTR_DEINIT;
	mutex_unlock(&eam_lock);

	eth_adaption_unregister_netdevice_notifier();
	eth_adaption_sb_unregister_listener();

	if(server)
		eth_adaption_server_cleanup();
	else
		eth_adaption_client_cleanup();

#ifdef CONFIG_MSM_BOOT_TIME_MARKER
	place_marker("M - eth-adaption-layer eth_adapt_exit");
#endif
}

/**
* eth_adaption_notifier_soft_reset() - resets Ethernet adaptation module.
* Return: void
*/
void eth_adaption_notifier_soft_reset(struct kthread_work *work)
{
	ETHADPTDBG("eth_adaption_notifier_soft_reset entry \n");
	if(qrtr_init == QRTR_DEINIT || qrtr_init == QRTR_INPROGRESS)
		return;

	/* If link is already down do not call QRTR status cb with down */
	if(qrtr_init == QRTR_INIT && link_state != NETDEV_DOWN)
		qcom_ethernet_qrtr_status_cb(NETDEV_DOWN);

	if(server)
	{
		eth_adaption_server_cleanup();
	}
	else
	{
		eth_adaption_client_cleanup();
	}
	ETHADPTDBG("eth_adaption_notifier_soft_reset exit \n");
}

/**
* eth_adaption_notifier_soft_set() - sets Ethernet adaptation module.
* Return: void
*/
void eth_adaption_notifier_soft_set(struct kthread_work *work)
{
	ETHADPTDBG("eth_adaption_notifier_soft_set entry \n");

	/*to avoid duplicate client connects if we receive two simultanoues UP events*/
	if(qrtr_init == QRTR_INIT || qrtr_init == QRTR_INPROGRESS)
		return;

	if(server)
	{
		eth_adaption_server_connect(dest_port,iptype,connect_retry_cnt);
	}
	else
	{
		if (iptype == 0)
		{
			eth_adaption_client_connect(destipv4,iptype,dest_port,connect_retry_cnt);
		}
		else
		{
			eth_adaption_client_connect(destipv6,iptype,dest_port,connect_retry_cnt);
		}
	}
	ETHADPTDBG("eth_adaption_notifier_soft_set exit \n");
}


module_init(eth_adaption_init)
module_exit(eth_adaption_exit)

MODULE_AUTHOR("Abhishek B Chauhan -QTI");
MODULE_DESCRIPTION("QTI Ethernet Adaptation Module");
MODULE_LICENSE("GPL v2");
