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
* client_connect() - Connect to the server on other Processor.
* Notify QRTR with link up status callback if
* connection success.
* Wait for Rx data and pass the buffer to
* QRTR.
* @destip: IPV4/IPV6 address of remote server.
* Return: Error code in failure case.
*/

int client_connect(unsigned char *destip ,int iptype, int port, int retry_cnt);

/**
* client_send() - this will be called from qrtr context.
* @buf: Buffer to send
* @length: Length of the buffer
* Return: Length of sent buffer.
*/
int client_send(const char *buf, const size_t length);

/**
* client_receive() - this will be called eal context.
* @sock:
* @str:
* Return: Received buffer length.
*/
static void client_receive(struct kthread_work *work);

/**
* client_cleanup() - this will be called eal context.
* @sock:
* @str:
* Return: Received buffer length.
*/
void client_cleanup(void);
