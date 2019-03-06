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

#define AQC_RX_TAIL_PTR_OFFSET 0x00005B10
#define AQC_RX_TAIL_PTR(base, idx) \
	(base + AQC_RX_TAIL_PTR_OFFSET + (idx * 0x20))

#define AQC_RX_HEAD_PTR_OFFSET 0x00005B0C
#define AQC_RX_HEAD_PTR(base, idx) \
	(base + AQC_RX_HEAD_PTR_OFFSET + (idx * 0x20))

#define AQC_TX_TAIL_PTR_OFFSET 0x00007C10
#define AQC_TX_TAIL_PTR(base, idx) \
	(base + AQC_TX_TAIL_PTR_OFFSET + (idx * 0x40))

#define AQC_TX_HEAD_PTR_OFFSET 0x00007C0C
#define AQC_TX_HEAD_PTR(base, idx) \
	(base + AQC_TX_HEAD_PTR_OFFSET + (idx * 0x40))

