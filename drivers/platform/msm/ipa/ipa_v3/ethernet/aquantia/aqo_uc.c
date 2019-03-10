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

#include "aqo_i.h"

// following should come from device tree?
#define UC_PROT_AQC 0x00000001

#define UC_DIR_AQC_RX 0
#define UC_DIR_AQC_TX 1

struct uc_cmd_param_aqc_init
{
	u32 periph_baddr_lsb;
	u32 periph_baddr_msb;
} __packed;

struct uc_cmd_param_aqc_setup
{
	u8 dir;
	u8 aqc_ch;
	u8 gsi_ch;
	u8 reserved;
} __packed;

struct uc_cmd_param_aqc_teardown
{
	u8 gsi_ch;
	u8 reserved_0;
	u16 reserved_1;
} __packed;

struct uc_cmd_param_aqc_deinit
{
	u32 reserved;
} __packed;

int aqo_uc_init_peripheral(u64 per_base)
{
	int rc;
	struct uc_cmd_param_aqc_init param;

	pr_crit("AQC: aqo_uc_init_rx started");

	memset(&param, 0, sizeof(param));

	param.periph_baddr_lsb = lower_32_bits(per_base);
	param.periph_baddr_msb = upper_32_bits(per_base);

	pr_crit("AQC: param.periph_baddr_lsb=%x", param.periph_baddr_lsb);
	pr_crit("AQC: param.periph_baddr_msb=%x", param.periph_baddr_msb);

	rc = ipa_eth_uc_send_cmd(IPA_ETH_UC_OP_PER_INIT, UC_PROT_AQC,
			&param, sizeof(param));
	if (rc) {
		pr_crit("AQC: ipa_eth_uc_send_cmd failed");
	} else {
		pr_crit("AQC: ipa_eth_uc_send_cmd successful");
	}

	return rc;
}

int aqo_uc_setup_channel(bool tx, u8 aqc_ch, u8 gsi_ch)
{
	int rc;
	struct uc_cmd_param_aqc_setup param_setup;

	pr_crit("AQC: aqo_uc_start_rx started");

	memset(&param_setup, 0, sizeof(param_setup));

	param_setup.dir = tx ? UC_DIR_AQC_TX : UC_DIR_AQC_RX;
	param_setup.aqc_ch = aqc_ch;
	param_setup.gsi_ch = gsi_ch;

	rc = ipa_eth_uc_send_cmd(IPA_ETH_UC_OP_CH_SETUP, UC_PROT_AQC,
			&param_setup, sizeof(param_setup));
	if (rc) {
		pr_crit("AQC: ipa_eth_uc_send_cmd setup failed");
	} else {
		pr_crit("AQC: ipa_eth_uc_send_cmd setup successful");
	}

	return rc;
}

int aqo_uc_teardown_channel(u8 gsi_ch)
{
	int rc;
	struct uc_cmd_param_aqc_teardown param_teardown;

	pr_crit("AQC: aqo_uc_stop_rx started");

	memset(&param_teardown, 0, sizeof(param_teardown));

	param_teardown.gsi_ch = gsi_ch;

	rc = ipa_eth_uc_send_cmd(IPA_ETH_UC_OP_CH_TEARDOWN, UC_PROT_AQC,
			&param_teardown, sizeof(param_teardown));
	if (rc) {
		pr_crit("AQC: ipa_eth_uc_send_cmd teardown failed");
	} else {
		pr_crit("AQC: ipa_eth_uc_send_cmd teardown successful");
	}

	return rc;
}

int aqo_uc_deinit_peripheral(void)
{
	int rc;
	struct uc_cmd_param_aqc_deinit param_deinit;

	pr_crit("AQC: aqo_uc_deinit_rx started");

	memset(&param_deinit, 0, sizeof(param_deinit));

	rc = ipa_eth_uc_send_cmd(IPA_ETH_UC_OP_PER_DEINIT, UC_PROT_AQC,
			&param_deinit, sizeof(param_deinit));
	if (rc) {
		pr_crit("AQC: ipa_eth_uc_send_cmd teardown failed");
	} else {
		pr_crit("AQC: ipa_eth_uc_send_cmd teardown successful");
	}

	return rc;
}
