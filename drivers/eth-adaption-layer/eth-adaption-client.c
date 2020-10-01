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
#include <eth-adaption-client.h>
#include <soc/qcom/qrtr_ethernet.h>

#define IPV4_ADDR_LEN 32
#define IPV6_ADDR_LEN 48

extern unsigned long receive_allocfree_stat;
extern unsigned long send_data;
extern unsigned long recevied_data;
extern unsigned long error_stat;

struct client_socket
{
	struct socket *conn_socket;
	struct sockaddr_in *server;
	struct sockaddr_in6 *server_v6;
	struct kthread_worker kworker;
	struct task_struct *task;
	struct kthread_work init_client;
	struct kthread_work read_data;
	unsigned char *destip;
	int iptype;
	int port;
	int connect_retry_cnt;
	bool kpi_send_data;
	bool kpi_receive_data;
};

struct client_socket client_sk;

struct qrtr_ethernet_cb_info *cb_info_client;

struct ip_params
{
	struct in6_ifreq ipv6_addr;
	struct in_addr ipv4_addr;
	char ipv4_addr_str[IPV4_ADDR_LEN];
	char ipv6_addr_str[IPV6_ADDR_LEN];
};

struct ip_params pparams = {0};

extern struct eth_adapt_device eth_dev;
extern struct eth_adapt_result eth_res;
extern int qrtr_init;
extern struct mutex eam_lock;

/**
* eth_adaption_client_send() - this will be called from qrtr context.
* @buf: Buffer to send
* @length: Length of the buffer
* Return: Length of sent buffer.
*/
int eth_adaption_client_send(const char *buf, const size_t length)
{
	struct msghdr msg;
	struct kvec vec;
	int len = 0, written = 0, left = length;
	mm_segment_t oldmm;
	unsigned long flags = MSG_DONTWAIT;

	msg.msg_name    = 0;
	msg.msg_namelen = 0;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_flags   = flags;

  oldmm = get_fs(); set_fs(KERNEL_DS);
repeat_send:
  vec.iov_len = left;
  vec.iov_base = (char *)buf + written;

	len = kernel_sendmsg(client_sk.conn_socket, &msg, &vec, left, left);
	if((len == -ERESTARTSYS) || (!(flags & MSG_DONTWAIT) && (len == -EAGAIN)))
		goto repeat_send;
	if(len > 0)
	{
		written += len;
		left -= len;
		if(left)
			goto repeat_send;
	}
set_fs(oldmm);
	if(client_sk.kpi_send_data)
	{
#ifdef CONFIG_MSM_BOOT_TIME_MARKER
	place_marker("M - eth-adaption-layer client_send first packets");
#endif
		client_sk.kpi_send_data =false;
	}
	send_data+=(written ? written:len);
	return written ? written:len;
} /* client_send */

/**
* eth_adaption_client_receive() - this will be called eal context.
* @work: kthread work
* Return: void.
*/
static void eth_adaption_client_receive(struct kthread_work *work)
{
	struct msghdr msg;
	struct kvec vec;
	int len;
	int max_size = MAX_SIZE;
	void *buf;

	buf = kzalloc(max_size, GFP_ATOMIC);
	if (!buf)
		return;
	if(!client_sk.conn_socket)
		goto release;

	msg.msg_name    = 0;
	msg.msg_namelen = 0;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_flags   = MSG_DONTWAIT;
	vec.iov_len = max_size;
	vec.iov_base = buf;

	receive_allocfree_stat++;
	len = kernel_recvmsg(client_sk.conn_socket, &msg, &vec, max_size, max_size, msg.msg_flags);

	if(len == -EAGAIN || len == -ERESTARTSYS)
	{
		ETHADPTDBG("Failure to read QRTR packets kernel error code %d\n",len);
		error_stat+=len;
		goto release;
	}

	eth_res.buf_addr = buf;
	eth_res.bytes_xferd = len;
	// send this message to qrtr.
	qcom_ethernet_qrtr_dl_cb(&eth_res);
	ETHADPTDBG("the server says: %x %d| client_receive\n", buf,len);
	if(client_sk.kpi_receive_data)
	{
#ifdef CONFIG_MSM_BOOT_TIME_MARKER
	place_marker("M - eth-adaption-layer client_receive first packets");
#endif
	client_sk.kpi_receive_data =false;
	}
	recevied_data+=len;
	receive_allocfree_stat--;

release:
	kfree(buf);
	return;
} /* client_receive */

/**
* eth_adaption_client_data_ready() - this will be called init context if skb is ready.
* @sk: client socket
* Return:void.
*/
static void eth_adaption_client_data_ready(struct sock *sk)
{
	ETHADPTDBG("kernel queue work called\n");
	kthread_queue_work(&client_sk.kworker, &client_sk.read_data);
}

/**
* eth_adaption_client_start() - Connect to the server on other Processor.
* Notify QRTR with link up status callback if
* connection success.
* Wait for Rx data and pass the buffer to
* QRTR.
* @work: kthread work.
* Return: void.
*/
static void eth_adaption_client_start(struct kthread_work *work)
{
	struct socket *sockt;
	struct sockaddr_in *server = NULL;
	struct sockaddr_in6 *server_v6 = NULL;
	int acc,cn,ret,sent,count;
	int tcpnodelay = 1;

	if(client_sk.iptype == 0)
	{
		server=(struct sockaddr_in*) kmalloc(sizeof(struct sockaddr_in),GFP_KERNEL);
		acc=sock_create(AF_INET, SOCK_STREAM, IPPROTO_TCP, &sockt);
	}
	else if(client_sk.iptype == 1)
	{
		server_v6 = (struct sockaddr_in6*) kmalloc(sizeof(struct sockaddr_in6),GFP_KERNEL);
		acc=sock_create(AF_INET6, SOCK_STREAM, IPPROTO_TCP, &sockt);
	}
	cb_info_client =(struct qrtr_ethernet_cb_info *) kmalloc(sizeof(struct qrtr_ethernet_cb_info),GFP_KERNEL);

	ETHADPTDBG("client sock %d\n");
	if(acc < 0)
	{
		printk(KERN_ALERT "socket failed %d\n",acc);
		goto release;
	}

	acc = kernel_setsockopt(sockt, SOL_TCP, TCP_NODELAY, (char *)&tcpnodelay, sizeof(tcpnodelay));

	if (acc < 0)
	{
		ETHADPTDBG("Can`t set a socket option TCP_NODELAY %d\n", acc);
		goto release;
	}

	if(client_sk.iptype == 0)
	{
		strlcpy(pparams.ipv4_addr_str,
		client_sk.destip, sizeof(pparams.ipv4_addr_str));
		ETHADPTDBG("client IPv4 addr %s\n",pparams.ipv4_addr_str);

		ret = in4_pton(pparams.ipv4_addr_str, -1,
			       (u8 *)&pparams.ipv4_addr.s_addr, -1, NULL);

		if (ret != 1 || pparams.ipv4_addr.s_addr == 0)
		{
			ETHADPTERR("Invalid ipv4 address programmed: %s\n",
				  client_sk.destip);
			goto release;
		}

		memset(server,0,sizeof(struct sockaddr_in));
		server->sin_family = AF_INET;
		server->sin_addr.s_addr = htonl(pparams.ipv4_addr.s_addr);
		memcpy(&server->sin_addr.s_addr, &pparams.ipv4_addr,
		   sizeof(server->sin_addr.s_addr));
		server->sin_port = htons(client_sk.port);
	}
	else if (client_sk.iptype == 1)
	{
		strlcpy(pparams.ipv6_addr_str,
		client_sk.destip, sizeof(pparams.ipv6_addr_str));
		ETHADPTDBG("eth_adapt_init ethernet IPv6 addr: %s\n", pparams.ipv6_addr_str);

		ret = in6_pton(pparams.ipv6_addr_str, -1,
			(u8 *)&pparams.ipv6_addr.ifr6_addr.s6_addr32, -1, NULL);

		if (ret != 1 || !pparams.ipv6_addr.ifr6_addr.s6_addr32)
		{
			ETHADPTERR("Invalid ipv6 address programmed: %s\n",
				  client_sk.destip);
			goto release;
		}

		memset(server_v6,0,sizeof(struct sockaddr_in6));
		server_v6->sin6_family = AF_INET6;
		memcpy(&server_v6->sin6_addr.s6_addr, &pparams.ipv6_addr.ifr6_addr.s6_addr32,
		   sizeof(server_v6->sin6_addr.s6_addr));
		server_v6->sin6_port = htons(client_sk.port);
	}

connect:

	if (client_sk.iptype == 0)
	{
		cn=kernel_connect(sockt, (struct sockaddr*) server,sizeof(struct sockaddr_in),O_RDWR);
	}
	else if (client_sk.iptype == 1)
	{
		cn=kernel_connect(sockt, (struct sockaddr*) server_v6,sizeof(struct sockaddr_in6),O_RDWR);
	}
	ETHADPTINFO("kernel connection client code::%d\n",cn);

	if(cn == 0)
	{
		client_sk.conn_socket = sockt;
		client_sk.server = server;
		client_sk.server_v6 = server_v6;
		client_sk.kpi_receive_data = true;
		client_sk.kpi_send_data  = true;
		cb_info_client->eth_send = eth_adaption_send;
		qcom_ethernet_init_cb(cb_info_client);

		/* Critical section */
		mutex_lock(&eam_lock);
		qrtr_init = QRTR_INIT;
		mutex_unlock(&eam_lock);

		kthread_init_work(&client_sk.read_data, eth_adaption_client_receive);
		client_sk.conn_socket->sk->sk_data_ready = eth_adaption_client_data_ready;
#ifdef CONFIG_MSM_BOOT_TIME_MARKER
		place_marker("M - eth-adaption-layer client_connect connected");
#endif
	}
	else
	{
		DECLARE_WAIT_QUEUE_HEAD(connect_retry_wait);
		if (client_sk.connect_retry_cnt > 0)
		{
			 wait_event_timeout(connect_retry_wait, 0 ,5*HZ);
			 --client_sk.connect_retry_cnt;
			 goto connect;
		}
		ETHADPTERR("kernel connection client failed code::%d\n",cn);
#ifdef CONFIG_MSM_BOOT_TIME_MARKER
				place_marker("M - eth-adaption-layer client_connect failed");
#endif
		goto release;
	}

return cn;
	// Call qrtr to initialize endpoint and pass the eth_adapt_send fn ptr to qrtr

release:
	/* Critical section */
	mutex_lock(&eam_lock);
	qrtr_init = QRTR_CONNFAILED;
	mutex_unlock(&eam_lock);

	if(server)
		kfree(server);
	if(server_v6)
		kfree(server_v6);
	return cn;
}

/**
* eth_adaption_client_connect() - Connect to the server on other Processor.
* Notify QRTR with link up status callback if
* connection success.
* Wait for Rx data and pass the buffer to
* QRTR.
* @destip:
* @iptype:
* @port:
* @connect_retry_cnt:
* Return: Error code in failure case.
*/
int eth_adaption_client_connect(unsigned char *destip, int iptype, int port,int connect_retry_cnt)
{
	unsigned int ipaddr_len;

	/*First thing you need to do is MUTEX init*/
	/*Do not add any code above this comment*/
	mutex_init(&eam_lock);

	if (iptype == 0)
		ipaddr_len = IPV4_ADDR_LEN;
	else
		ipaddr_len = IPV6_ADDR_LEN;

	client_sk.destip = destip;
	client_sk.iptype = iptype;
	client_sk.port = port;
	client_sk.connect_retry_cnt = connect_retry_cnt;

	/* Critical section */
	mutex_lock(&eam_lock);
	qrtr_init = QRTR_INPROGRESS;
	mutex_unlock(&eam_lock);

	kthread_init_work(&client_sk.init_client, eth_adaption_client_start);
	kthread_init_worker(&client_sk.kworker);
	client_sk.task = kthread_run(kthread_worker_fn, &client_sk.kworker, "eth_adapt_rx");
	if (IS_ERR(client_sk.task))
	{
		ETHADPTERR("%s: Error allocating wq\n", __func__);
		return;
	}

	kthread_queue_work(&client_sk.kworker, &client_sk.init_client);
#ifdef CONFIG_MSM_BOOT_TIME_MARKER
	place_marker("M - eth-adaption-layer client_connect init");
#endif

	return 0;
}

/**
* eth_adaption_client_cleanup() - this will be called eal context to cleanup module.
* Return: void
*/
void eth_adaption_client_cleanup(void)
{
	ETHADPTINFO(KERN_ALERT"client_cleanup entry\n");
	/* Critical section */
	mutex_lock(&eam_lock);
	qrtr_init = QRTR_DEINIT;
	mutex_unlock(&eam_lock);

	/*reset packet stats*/
	send_data = 0;
	recevied_data = 0;
	receive_allocfree_stat = 0;
	error_stat = 0;

	if(client_sk.task)
	{
		kthread_cancel_work_sync(&client_sk.read_data);
		kthread_cancel_work_sync(&client_sk.init_client);
		kthread_flush_work(&client_sk.init_client);
		kthread_flush_work(&client_sk.read_data);
		kthread_flush_worker(&client_sk.kworker);
		kthread_stop(client_sk.task);
		client_sk.task = NULL;
	}

	if(client_sk.conn_socket)
	{
		kernel_sock_shutdown(client_sk.conn_socket,SHUT_RDWR);
		tcp_abort(client_sk.conn_socket->sk, ENODEV);
	}

	if(client_sk.conn_socket)
	{
		sock_release(client_sk.conn_socket);
		client_sk.conn_socket = NULL;
	}

	if(cb_info_client)
	{
		kfree(cb_info_client);
		cb_info_client = NULL;
	}

	if(client_sk.server)
	{
		kfree(client_sk.server);
		client_sk.server = NULL;
	}

	if(client_sk.server_v6)
	{
		kfree(client_sk.server_v6);
		client_sk.server_v6 = NULL;
	}
	ETHADPTINFO(KERN_ALERT"client_cleanup exit\n");
	mutex_destroy(&eam_lock);
}
