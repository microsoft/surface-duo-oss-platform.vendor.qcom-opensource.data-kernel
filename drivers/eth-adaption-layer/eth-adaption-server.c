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
#include <soc/qcom/qrtr_ethernet.h>

extern unsigned long receive_allocfree_stat;
extern unsigned long send_data;
extern unsigned long recevied_data;
extern unsigned long error_stat;


struct server_socket
{
	struct socket *sock;
	struct socket *newsocket;
	struct sockaddr_in *server;
	struct sockaddr_in6 *serverv6;
	struct kthread_worker kworker;
	struct task_struct *task;
	struct kthread_work init_server;
	struct kthread_work read_data;
	struct qrtr_ethernet_cb_info *cb_info_server;
	int server_port;
	int iptype;
	int connect_retry_cnt;
	bool rmmod;
	bool kpi_send_data;
	bool kpi_receive_data;
};

struct server_socket serv_sk;

extern struct eth_adapt_device eth_dev;
extern struct eth_adapt_result eth_res;
extern int qrtr_init;
extern struct mutex eam_lock;

struct qrtr_ethernet_cb_info *cb_info_server;

/**
* eth_adaption_server_send() - Function to send QMI packet from IPCRTR over
* TCP socket.
*
* @buf: Buffer holding QMI message.
* @length: Buffer length
*
* Return: Length of the buffer sent.
*/

int eth_adaption_server_send(const char *buf, const size_t length)
{
	struct msghdr msg;
	struct kvec vec;
	int len, written = 0, left =length;
	mm_segment_t oldmm;
	unsigned long flags = MSG_DONTWAIT;

	if(!serv_sk.newsocket)
		return written;
	msg.msg_name    = 0;
	msg.msg_namelen = 0;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_flags = flags;

	oldmm = get_fs(); set_fs(KERNEL_DS);

repeat_send:
	vec.iov_len = left;
	vec.iov_base = (char *)buf + written;

	len = kernel_sendmsg(serv_sk.newsocket, &msg, &vec, left, left);

	if((len == -ERESTARTSYS) || (!(flags & MSG_DONTWAIT) &&\
		(len == -EAGAIN)))
		goto repeat_send;

	if(len > 0)
	{
		written += len;
		left -= len;
		if(left)
			goto repeat_send;
	}

	set_fs(oldmm);
	if(serv_sk.kpi_send_data)
	{
#ifdef CONFIG_MSM_BOOT_TIME_MARKER
	place_marker("M - eth-adaption-layer server_send first packets");
#endif
		serv_sk.kpi_send_data =false;
	}
	send_data+=(written?written:len);
	return written?written:len;
}

/**
* eth_adaption_server_receive() - Function to receive QMI packet over
* TCP socket.
* @work: kworker context.
*
* Use this API from Ethernet Adaptation.
*
* Return: void.
*/
static void eth_adaption_server_receive(struct kthread_work *work)
{
	struct msghdr msg;
	struct kvec vec;
	int len = 0;
	int max_size = MAX_SIZE;
	int sent;
	void *buf;

	ETHADPTDBG(KERN_ALERT "kernel server_receive called\n");
	buf = kzalloc(max_size, GFP_ATOMIC);
	if (!buf)
		return;
	if(!serv_sk.newsocket)
		goto release;

	receive_allocfree_stat++;
	msg.msg_name = 0;
	msg.msg_namelen = 0;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_flags = MSG_DONTWAIT;

	vec.iov_len = max_size;
	vec.iov_base = buf;

	len = kernel_recvmsg(serv_sk.newsocket, &msg, &vec, max_size, max_size, MSG_DONTWAIT);

	if(len == -EAGAIN || len == -ERESTARTSYS)
	{
		ETHADPTDBG("Failure to read QRTR packets kernel error code %d\n",len);
		error_stat+=len;
		goto release;
	}
	else
	{
	//send it to qrtr
		eth_res.buf_addr = buf;
		eth_res.bytes_xferd = len;
		qcom_ethernet_qrtr_dl_cb(&eth_res);
		ETHADPTDBG("the server says: %x %d| client_receive\n", buf,len);
	}
	if(serv_sk.kpi_receive_data)
	{
#ifdef CONFIG_MSM_BOOT_TIME_MARKER
		place_marker("M - eth-adaption-layer server_receive first packets");
#endif
		serv_sk.kpi_receive_data =false;
	}
	receive_allocfree_stat--;
	recevied_data+=len;
release:
kfree(buf);
}


/**
* eth_adaption_server_data_ready() - this will be called init context if skb is ready.
* @sk: server socket
* Return:void.
*/

static void eth_adaption_server_data_ready(struct sock *sk)
{
	ETHADPTDBG("kernel queue work called\n");
	//queue_work(serv_sk.workqueue, &serv_sk.work);
	kthread_queue_work(&serv_sk.kworker, &serv_sk.read_data);
}

/**
* eth_adaption_server_start() - Server initialization.
*
* Return: 0 for Success, error code otherwise.
*/
static void eth_adaption_server_start(struct kthread_work *work)
{
	struct socket *sock;
	struct socket *client = NULL;
	struct sockaddr_in *server = NULL;
	struct sockaddr_in6 *serverv6 = NULL;
	int reuseaddr = 1;
	int reuseport = 1;
	int tcpnodelay = 1;
	int error,bin,listen;
	int cn;
	int count = 0;
	bool ret;

	if (serv_sk.iptype == 0)
	{
	/* IPV4 */
	/* Create socket */
		error=sock_create(AF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
		server = (struct sockaddr_in *) kmalloc(sizeof(struct sockaddr_in),GFP_ATOMIC);
	}
	else if (serv_sk.iptype == 1)
	{
	/* IPV6 */
		error=sock_create(AF_INET6, SOCK_STREAM, IPPROTO_TCP, &sock);
		serverv6 = (struct sockaddr_in6 *) kmalloc(sizeof(struct sockaddr_in6),GFP_ATOMIC);
	}

	allow_signal(SIGKILL|SIGTERM);
	if (error < 0)
	{
		ETHADPTERR("\nCan`t create a socket%d\n",error);
	}

	error = kernel_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&reuseaddr, sizeof(reuseaddr));
	if (error < 0)
	{
		ETHADPTERR("Can`t set a socket option SO_REUSEADDR  %d\n", error);
	}
	error = kernel_setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (char *)&reuseport, sizeof(reuseport));

	if (error < 0)
	{
		ETHADPTERR("Can`t set a socket option SO_REUSEPORT %d\n", error);
	}

	client = sock_alloc();

	if (client == NULL)
		goto release;

	cb_info_server =(struct qrtr_ethernet_cb_info *) kmalloc(sizeof(struct qrtr_ethernet_cb_info),GFP_KERNEL);

	if(!cb_info_server)
	{
		ETHADPTERR("kmalloc failed ");
		goto release;
	}

	if(serv_sk.iptype == 0)
	{
		memset(server,0,sizeof(struct sockaddr_in));
		server->sin_family = AF_INET;
		server->sin_port = htons(serv_sk.server_port);
		server->sin_addr.s_addr = htonl(INADDR_ANY);
		bin=kernel_bind(sock,(struct sockaddr*) server,sizeof(struct sockaddr_in));
	}
	else if (serv_sk.iptype == 1)
	{
		memset(serverv6, 0, sizeof(struct sockaddr_in6));
		serverv6->sin6_family = AF_INET6;
		serverv6->sin6_addr = (in6addr_any);
		serverv6->sin6_port = htons(serv_sk.server_port);
		bin=kernel_bind(sock,(struct sockaddr*) serverv6,sizeof(struct sockaddr_in6));
	}

	if(bin<0)
	{
		ETHADPTERR("\nkernel bind failed\n %d", bin);
		goto release;
	}

	listen= kernel_listen(sock,1);
	if(listen<0)
	{
		ETHADPTERR("kernel listen failed %d\n", listen);
		goto release;
	}

	while((cn = kernel_accept(sock,&client,O_NONBLOCK)) < 0 && serv_sk.rmmod == false)
	{
		//Do nothing
	}
	ETHADPTINFO("kernel accept succeeded %d cn %d\n",serv_sk.rmmod,cn);

	if(cn == 0)
	{
		serv_sk.sock = sock;
		serv_sk.newsocket = client;
		serv_sk.cb_info_server = cb_info_server;

		if(serv_sk.iptype == 0)
		{
			serv_sk.server = server;
		}
		else if (serv_sk.iptype == 1)
		{
			serv_sk.serverv6 = serverv6;
		}
		error = kernel_setsockopt(client, SOL_TCP, TCP_NODELAY, (char *)&tcpnodelay, sizeof(tcpnodelay));
		if (error < 0)
		{
			ETHADPTERR(KERN_ALERT "Can`t set a socket option TCP_NODELAY %d\n", error);
			return;
		}
		// Call qrtr to initialize endpoint and pass the eth_adapt_send fn ptr to qrtr
		cb_info_server->eth_send = eth_adaption_send;
		qcom_ethernet_init_cb(cb_info_server);

		/* Critical section */
		mutex_lock(&eam_lock);
		qrtr_init = QRTR_INIT;
		mutex_unlock(&eam_lock);

		kthread_init_work(&serv_sk.read_data, eth_adaption_server_receive);
		serv_sk.newsocket->sk->sk_data_ready = eth_adaption_server_data_ready;
#ifdef CONFIG_MSM_BOOT_TIME_MARKER
			place_marker("M - eth-adaption-layer server_start connected");
#endif
	}

return cn;

release:
	/* Critical section */
	mutex_lock(&eam_lock);
	qrtr_init = QRTR_CONNFAILED;
	mutex_unlock(&eam_lock);

	if(server)
		kfree(server);
	if(serverv6)
		kfree(serverv6);
	return cn;
}

/**
* eth_adaption_server_connect() - Function to start server for sending
  QMI packet from IPCRTR over
* TCP socket.
*
* @port: port
* @iptype: ip protocol
* @retry_count retry count required.
* Return: Length of the buffer sent.
*/
int eth_adaption_server_connect(int port,int iptype,int connect_retry_cnt)
{
	/*First thing you need to do is MUTEX init*/
	/*Do not add any code above this comment*/
	mutex_init(&eam_lock);
	serv_sk.server_port = port;
	serv_sk.iptype = iptype;
	serv_sk.connect_retry_cnt = connect_retry_cnt;
	serv_sk.rmmod = false;
	serv_sk.kpi_receive_data = true;
	serv_sk.kpi_send_data  = true;

	/* Critical section */
	mutex_lock(&eam_lock);
	qrtr_init = QRTR_INPROGRESS;
	mutex_unlock(&eam_lock);

	kthread_init_work(&serv_sk.init_server, eth_adaption_server_start);
	kthread_init_worker(&serv_sk.kworker);
	serv_sk.task = kthread_run(kthread_worker_fn, &serv_sk.kworker, "eth_adapt_rx");
	if (IS_ERR(serv_sk.task))
	{
		ETHADPTERR("%s: Error allocating wq\n", __func__);
		return -1;
	}
	kthread_queue_work(&serv_sk.kworker, &serv_sk.init_server);
#ifdef CONFIG_MSM_BOOT_TIME_MARKER
		place_marker("M - eth-adaption-layer server_init");
#endif
	return 0;
}

/**
* eth_adaption_server_cleanup() - Function to clean up module
* @void: void.
* Return: void.
*/
void eth_adaption_server_cleanup(void)
{
	ETHADPTINFO("server_cleanup entry \n");
	serv_sk.rmmod = true;

	/* Critical section */
	mutex_lock(&eam_lock);
	qrtr_init = QRTR_DEINIT;
	mutex_unlock(&eam_lock);

	/*reset packet stats*/
	send_data = 0;
	recevied_data = 0;
	receive_allocfree_stat = 0;
	error_stat = 0;
	if (serv_sk.task)
	{
		kthread_cancel_work_sync(&serv_sk.read_data);
		kthread_cancel_work_sync(&serv_sk.init_server);
		kthread_flush_work(&serv_sk.init_server);
		kthread_flush_work(&serv_sk.read_data);
		kthread_flush_worker(&serv_sk.kworker);
		kthread_stop(serv_sk.task);
		serv_sk.task = NULL;
	}

	if(serv_sk.newsocket)
	{
		kernel_sock_shutdown(serv_sk.newsocket,SHUT_RDWR);
		tcp_abort(serv_sk.newsocket->sk, ENODEV);
	}

	if (serv_sk.sock)
	{
		sock_release(serv_sk.sock);
		serv_sk.sock  = NULL;
	}

	if (serv_sk.newsocket)
	{
		sock_release(serv_sk.newsocket);
		serv_sk.newsocket  = NULL;
	}

	if (serv_sk.cb_info_server)
	{
		kfree(serv_sk.cb_info_server);
		serv_sk.cb_info_server = NULL;
	}

	if (serv_sk.server)
	{
		kfree(serv_sk.server);
		serv_sk.server = NULL;
	}

	if (serv_sk.serverv6)
	{
		kfree(serv_sk.serverv6);
		serv_sk.serverv6 = NULL;
	}
	ETHADPTINFO("server_cleanup exit \n");
	mutex_destroy(&eam_lock);
}

