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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>

#include <linux/net.h>
#include <linux/tcp.h>
#include <linux/in.h>
#include <linux/socket.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <asm/uaccess.h>
#include <net/sock.h>

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
int eth_adaption_server_connect(int port,int iptype,int connect_retry_cnt);

/**
* eth_adaption_server_send() - Function to send QMI packet from IPCRTR over
* TCP socket.
*
* @buf: Buffer holding QMI message.
* @length: Buffer length
*
* Return: Length of the buffer sent.
*/
int eth_adaption_server_send(const char *buf, const size_t length);

/**
* eth_adaption_server_cleanup() - Function to clean up module
* @void: void.
* Return: void.
*/
void eth_adaption_server_cleanup(void);
