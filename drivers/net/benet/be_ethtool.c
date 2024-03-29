/*
 * Copyright (C) 2005 - 2009 ServerEngines
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.  The full GNU General
 * Public License is included in this distribution in the file called COPYING.
 *
 * Contact Information:
 * linux-drivers@serverengines.com
 *
 * ServerEngines
 * 209 N. Fair Oaks Ave
 * Sunnyvale, CA 94085
 */

#include "be.h"
#include "be_cmds.h"
#include <linux/ethtool.h>

struct be_ethtool_stat {
	char desc[ETH_GSTRING_LEN];
	int type;
	int size;
	int offset;
};

enum {NETSTAT, PORTSTAT, MISCSTAT, DRVSTAT, ERXSTAT};
#define FIELDINFO(_struct, field) FIELD_SIZEOF(_struct, field), \
					offsetof(_struct, field)
#define NETSTAT_INFO(field) 	#field, NETSTAT,\
					FIELDINFO(struct net_device_stats,\
						field)
#define DRVSTAT_INFO(field) 	#field, DRVSTAT,\
					FIELDINFO(struct be_drvr_stats, field)
#define MISCSTAT_INFO(field) 	#field, MISCSTAT,\
					FIELDINFO(struct be_rxf_stats, field)
#define PORTSTAT_INFO(field) 	#field, PORTSTAT,\
					FIELDINFO(struct be_port_rxf_stats, \
						field)
#define ERXSTAT_INFO(field) 	#field, ERXSTAT,\
					FIELDINFO(struct be_erx_stats, field)

static const struct be_ethtool_stat et_stats[] = {
	{NETSTAT_INFO(rx_packets)},
	{NETSTAT_INFO(tx_packets)},
	{NETSTAT_INFO(rx_bytes)},
	{NETSTAT_INFO(tx_bytes)},
	{NETSTAT_INFO(rx_errors)},
	{NETSTAT_INFO(tx_errors)},
	{NETSTAT_INFO(rx_dropped)},
	{NETSTAT_INFO(tx_dropped)},
	{DRVSTAT_INFO(be_tx_reqs)},
	{DRVSTAT_INFO(be_tx_stops)},
	{DRVSTAT_INFO(be_fwd_reqs)},
	{DRVSTAT_INFO(be_tx_wrbs)},
	{DRVSTAT_INFO(be_rx_polls)},
	{DRVSTAT_INFO(be_tx_events)},
	{DRVSTAT_INFO(be_rx_events)},
	{DRVSTAT_INFO(be_tx_compl)},
	{DRVSTAT_INFO(be_rx_compl)},
	{DRVSTAT_INFO(be_ethrx_post_fail)},
	{DRVSTAT_INFO(be_802_3_dropped_frames)},
	{DRVSTAT_INFO(be_802_3_malformed_frames)},
	{DRVSTAT_INFO(be_tx_rate)},
	{DRVSTAT_INFO(be_rx_rate)},
	{PORTSTAT_INFO(rx_unicast_frames)},
	{PORTSTAT_INFO(rx_multicast_frames)},
	{PORTSTAT_INFO(rx_broadcast_frames)},
	{PORTSTAT_INFO(rx_crc_errors)},
	{PORTSTAT_INFO(rx_alignment_symbol_errors)},
	{PORTSTAT_INFO(rx_pause_frames)},
	{PORTSTAT_INFO(rx_control_frames)},
	{PORTSTAT_INFO(rx_in_range_errors)},
	{PORTSTAT_INFO(rx_out_range_errors)},
	{PORTSTAT_INFO(rx_frame_too_long)},
	{PORTSTAT_INFO(rx_address_match_errors)},
	{PORTSTAT_INFO(rx_vlan_mismatch)},
	{PORTSTAT_INFO(rx_dropped_too_small)},
	{PORTSTAT_INFO(rx_dropped_too_short)},
	{PORTSTAT_INFO(rx_dropped_header_too_small)},
	{PORTSTAT_INFO(rx_dropped_tcp_length)},
	{PORTSTAT_INFO(rx_dropped_runt)},
	{PORTSTAT_INFO(rx_fifo_overflow)},
	{PORTSTAT_INFO(rx_input_fifo_overflow)},
	{PORTSTAT_INFO(rx_ip_checksum_errs)},
	{PORTSTAT_INFO(rx_tcp_checksum_errs)},
	{PORTSTAT_INFO(rx_udp_checksum_errs)},
	{PORTSTAT_INFO(rx_non_rss_packets)},
	{PORTSTAT_INFO(rx_ipv4_packets)},
	{PORTSTAT_INFO(rx_ipv6_packets)},
	{PORTSTAT_INFO(tx_unicastframes)},
	{PORTSTAT_INFO(tx_multicastframes)},
	{PORTSTAT_INFO(tx_broadcastframes)},
	{PORTSTAT_INFO(tx_pauseframes)},
	{PORTSTAT_INFO(tx_controlframes)},
	{MISCSTAT_INFO(rx_drops_no_pbuf)},
	{MISCSTAT_INFO(rx_drops_no_txpb)},
	{MISCSTAT_INFO(rx_drops_no_erx_descr)},
	{MISCSTAT_INFO(rx_drops_no_tpre_descr)},
	{MISCSTAT_INFO(rx_drops_too_many_frags)},
	{MISCSTAT_INFO(rx_drops_invalid_ring)},
	{MISCSTAT_INFO(forwarded_packets)},
	{MISCSTAT_INFO(rx_drops_mtu)},
	{ERXSTAT_INFO(rx_drops_no_fragments)},
};
#define ETHTOOL_STATS_NUM ARRAY_SIZE(et_stats)

static const char et_self_tests[][ETH_GSTRING_LEN] = {
	"MAC Loopback test",
	"PHY Loopback test",
	"External Loopback test",
	"DDR DMA test",
	"Link test"
};
#define BE_NUM_TESTS ARRAY_SIZE(et_self_tests)

static void
be_get_drvinfo(struct net_device *netdev, struct ethtool_drvinfo *drvinfo)
{
	struct be_adapter *adapter = netdev_priv(netdev);

	strcpy(drvinfo->driver, DRV_NAME);
	strcpy(drvinfo->version, DRV_VER);
	strncpy(drvinfo->fw_version, adapter->fw_ver, FW_VER_LEN);
	strcpy(drvinfo->bus_info, pci_name(adapter->pdev));
	drvinfo->testinfo_len = 0;
	drvinfo->regdump_len = 0;
	drvinfo->eedump_len = 0;
}

static int
be_get_coalesce(struct net_device *netdev, struct ethtool_coalesce *coalesce)
{
	struct be_adapter *adapter = netdev_priv(netdev);
	struct be_eq_obj *be_eq = &adapter->be_eq;


	coalesce->rx_coalesce_usecs = be_eq->cur_eqd;
	coalesce->rx_coalesce_usecs_high = be_eq->max_eqd;
	coalesce->rx_coalesce_usecs_low = be_eq->min_eqd;

	coalesce->tx_coalesce_usecs = be_eq->cur_eqd;
	coalesce->tx_coalesce_usecs_high = be_eq->max_eqd;
	coalesce->tx_coalesce_usecs_low = be_eq->min_eqd;

	coalesce->use_adaptive_rx_coalesce = be_eq->enable_aic;
	coalesce->use_adaptive_tx_coalesce = be_eq->enable_aic;

	return 0;
}

/*
 * This routine is used to set interrup coalescing delay
 */
static int
be_set_coalesce(struct net_device *netdev, struct ethtool_coalesce *coalesce)
{
	struct be_adapter *adapter = netdev_priv(netdev);
	struct be_eq_obj *be_eq = &adapter->be_eq;
	u32 max = 0, min = 0, cur = 0, rx, tx;
	int status = 0;

	/* if AIC is being turned on now, start with an EQD of 0 */
	if (be_eq->enable_aic == 0 &&
		((coalesce->use_adaptive_rx_coalesce == 1) || 
		(coalesce->use_adaptive_tx_coalesce == 1))) {
		be_eq->cur_eqd = 0;
	}

	/* We have a single EQ. Rx settings will override Tx settings */
	tx = (be_eq->enable_aic != coalesce->use_adaptive_tx_coalesce);
	rx = (be_eq->enable_aic != coalesce->use_adaptive_rx_coalesce);

	if (tx)
		be_eq->enable_aic = coalesce->use_adaptive_tx_coalesce;
	if (rx)
		be_eq->enable_aic = coalesce->use_adaptive_rx_coalesce;

	tx = (be_eq->max_eqd != coalesce->tx_coalesce_usecs_high);
	rx = (be_eq->max_eqd != coalesce->rx_coalesce_usecs_high);

	if (tx)
		max = coalesce->tx_coalesce_usecs_high;
	else if (rx)
		max = coalesce->rx_coalesce_usecs_high;
	else
		max = be_eq->max_eqd;

	tx = (be_eq->min_eqd != coalesce->tx_coalesce_usecs_low);
	rx = (be_eq->min_eqd != coalesce->rx_coalesce_usecs_low);

	if (tx)
		min = coalesce->tx_coalesce_usecs_low;
	else if (rx)
		min = coalesce->rx_coalesce_usecs_low;
	else
		min = be_eq->min_eqd;

	tx = (be_eq->cur_eqd != coalesce->tx_coalesce_usecs);
	rx = (be_eq->cur_eqd != coalesce->rx_coalesce_usecs);

	if (tx)
		cur = coalesce->tx_coalesce_usecs;
	else if (rx)
		cur = coalesce->rx_coalesce_usecs;
	else
		cur = be_eq->cur_eqd;

	if (be_eq->enable_aic) {
		if (max > BE_MAX_EQD)
			max = BE_MAX_EQD;
		if (min > max)
			min = max;
		be_eq->max_eqd = max;
		be_eq->min_eqd = min;
		if (be_eq->cur_eqd > max)
			be_eq->cur_eqd = max;
		if (be_eq->cur_eqd < min)
			be_eq->cur_eqd = min;
	} else {
		if (cur > BE_MAX_EQD)
			cur = BE_MAX_EQD;
		if (be_eq->cur_eqd != cur) {
			status = be_cmd_modify_eqd(adapter, be_eq->q.id, cur);
			if (!status)
				be_eq->cur_eqd = cur;
		}
	}
	return 0;
}

static u32 be_get_rx_csum(struct net_device *netdev)
{
	struct be_adapter *adapter = netdev_priv(netdev);

	return adapter->rx_csum;
}

static int be_set_rx_csum(struct net_device *netdev, uint32_t data)
{
	struct be_adapter *adapter = netdev_priv(netdev);

	if (data)
		adapter->rx_csum = true;
	else
		adapter->rx_csum = false;

	return 0;
}

static void
be_get_ethtool_stats(struct net_device *netdev,
		struct ethtool_stats *stats, uint64_t *data)
{
	struct be_adapter *adapter = netdev_priv(netdev);
	struct be_drvr_stats *drvr_stats = &adapter->stats.drvr_stats;
	struct be_hw_stats *hw_stats = hw_stats_from_cmd(adapter->stats.cmd.va);
	struct be_rxf_stats *rxf_stats = &hw_stats->rxf;
	struct be_port_rxf_stats *port_stats =
			&rxf_stats->port[adapter->port_num];
	struct net_device_stats *net_stats = &adapter->stats.net_stats;
	struct be_erx_stats *erx_stats = &hw_stats->erx;
	void *p = NULL;
	int i;

	for (i = 0; i < ETHTOOL_STATS_NUM; i++) {
		switch (et_stats[i].type) {
		case NETSTAT:
			p = net_stats;
			break;
		case DRVSTAT:
			p = drvr_stats;
			break;
		case PORTSTAT:
			p = port_stats;
			break;
		case MISCSTAT:
			p = rxf_stats;
			break;
		case ERXSTAT: /* Currently only one ERX stat is provided */
			p = (u32 *)erx_stats + adapter->rx_obj.q.id;
			break;
		}

		p = (u8 *)p + et_stats[i].offset;
		data[i] = (et_stats[i].size == sizeof(u64)) ?
				*(u64 *)p: *(u32 *)p;
	}

	return;
}

static void
be_get_stat_strings(struct net_device *netdev, uint32_t stringset,
		uint8_t *data)
{
	int i;
	switch (stringset) {
	case ETH_SS_STATS:
		for (i = 0; i < ETHTOOL_STATS_NUM; i++) {
			memcpy(data, et_stats[i].desc, ETH_GSTRING_LEN);
			data += ETH_GSTRING_LEN;
		}
		break;
	case ETH_SS_TEST:
		for (i = 0; i < BE_NUM_TESTS; i++) {
			memcpy(data, et_self_tests[i], ETH_GSTRING_LEN);
			data += ETH_GSTRING_LEN;
		}
		break;
	}
}

static int be_get_stats_count(struct net_device *netdev)
{
	return ETHTOOL_STATS_NUM;
}

static int be_get_settings(struct net_device *netdev, struct ethtool_cmd *ecmd)
{
	struct be_adapter *adapter = netdev_priv(netdev);

	int status;
	u8 mac_speed = 0, connector = 0;
	u16 link_speed = 0;
	bool link_up = false;

	if (adapter->link_speed < 0) {
		status = be_cmd_link_status_query(adapter, &link_up, &mac_speed,
						&link_speed);

		/* link_speed is in units of 10 Mbps */
		if (link_speed) {
			ecmd->speed = link_speed*10;
		} else {
			switch (mac_speed) {
			case PHY_LINK_SPEED_1GBPS:
				ecmd->speed = SPEED_1000;
				break;
			case PHY_LINK_SPEED_10GBPS:
				ecmd->speed = SPEED_10000;
				break;
			}
		}

		status = be_cmd_read_port_type(adapter, adapter->port_num,
						&connector);
		if (!status) {
			switch (connector) {
			case 7:
				ecmd->port = PORT_FIBRE;
				ecmd->transceiver = XCVR_EXTERNAL;
				break;
			case 0:
				ecmd->port = PORT_TP;
				ecmd->transceiver = XCVR_EXTERNAL;
				break;
			default:
				ecmd->port = PORT_TP;
				ecmd->transceiver = XCVR_INTERNAL;
				break;
			}
		} else {
			ecmd->port = PORT_AUI;
			ecmd->transceiver = XCVR_INTERNAL;
		}

		adapter->link_speed = ecmd->speed;
		adapter->port_type = ecmd->port;
		adapter->transceiver = ecmd->transceiver;
	} else {
		ecmd->speed = adapter->link_speed;
		ecmd->port = adapter->port_type;
		ecmd->transceiver = adapter->transceiver;
	}

	ecmd->duplex = DUPLEX_FULL;
	ecmd->autoneg = AUTONEG_DISABLE;
	ecmd->phy_address = adapter->port_num;
	switch (ecmd->port) {
	case PORT_FIBRE:
		ecmd->supported = (SUPPORTED_10000baseT_Full | SUPPORTED_FIBRE);
		break;
	case PORT_TP:
		ecmd->supported = (SUPPORTED_10000baseT_Full | SUPPORTED_TP);
		break;
	case PORT_AUI:
		ecmd->supported = (SUPPORTED_10000baseT_Full | SUPPORTED_AUI);
		break;
	}

	return 0;
}

static void
be_get_ringparam(struct net_device *netdev, struct ethtool_ringparam *ring)
{
	struct be_adapter *adapter = netdev_priv(netdev);

	ring->rx_max_pending = adapter->rx_obj.q.len;
	ring->tx_max_pending = adapter->tx_obj.q.len;

	ring->rx_pending = atomic_read(&adapter->rx_obj.q.used);
	ring->tx_pending = atomic_read(&adapter->tx_obj.q.used);
}

static void
be_get_pauseparam(struct net_device *netdev, struct ethtool_pauseparam *ecmd)
{
	struct be_adapter *adapter = netdev_priv(netdev);

	be_cmd_get_flow_control(adapter, &ecmd->tx_pause, &ecmd->rx_pause);
	ecmd->autoneg = 0;
}

static int
be_set_pauseparam(struct net_device *netdev, struct ethtool_pauseparam *ecmd)
{
	struct be_adapter *adapter = netdev_priv(netdev);
	int status;

	if (ecmd->autoneg != 0)
		return -EINVAL;
	adapter->tx_fc = ecmd->tx_pause;
	adapter->rx_fc = ecmd->rx_pause;

	status = be_cmd_set_flow_control(adapter,
					adapter->tx_fc, adapter->rx_fc);
	if (status)
		dev_warn(&adapter->pdev->dev, "Pause param set failed.\n");

	return status;
}

static int
be_phys_id(struct net_device *netdev, u32 data)
{
	struct be_adapter *adapter = netdev_priv(netdev);
	int status;
	u32 cur;

	be_cmd_get_beacon_state(adapter, adapter->port_num, &cur);

	if (cur == BEACON_STATE_ENABLED)
		return 0;

	if (data < 2)
		data = 2;

	status = be_cmd_set_beacon_state(adapter, adapter->port_num, 0, 0,
			BEACON_STATE_ENABLED);
	set_current_state(TASK_INTERRUPTIBLE);
	schedule_timeout(data*HZ);

	status = be_cmd_set_beacon_state(adapter, adapter->port_num, 0, 0,
			BEACON_STATE_DISABLED);


	return status;
}

static void
be_get_wol(struct net_device *netdev, struct ethtool_wolinfo *wol)
{
	struct be_adapter *adapter = netdev_priv(netdev);

	wol->supported = WAKE_MAGIC;
	if (adapter->wol)
		wol->wolopts = WAKE_MAGIC;
	else
		wol->wolopts = 0;
	memset(&wol->sopass, 0, sizeof(wol->sopass));
	return;
}

static int
be_set_wol(struct net_device *netdev, struct ethtool_wolinfo *wol)
{
	struct be_adapter *adapter = netdev_priv(netdev);

	if (wol->wolopts & ~WAKE_MAGIC)
		return -EINVAL;

	if (wol->wolopts & WAKE_MAGIC)
		adapter->wol = true;
	else
		adapter->wol = false;

	return 0;
}

static int
be_test_ddr_dma(struct be_adapter *adapter)
{
	int ret, i;
	struct be_dma_mem ddrdma_cmd;
	u64 pattern[2] = {0x5a5a5a5a5a5a5a5aLLU, 0xa5a5a5a5a5a5a5a5LLU};

	ddrdma_cmd.size = sizeof(struct be_cmd_req_ddrdma_test);
	ddrdma_cmd.va = pci_alloc_consistent(adapter->pdev, ddrdma_cmd.size,
			&ddrdma_cmd.dma);
	if (!ddrdma_cmd.va) {
		dev_err(&adapter->pdev->dev, "Memory allocation failure \n");
		return -ENOMEM;
	}

	for (i = 0; i < 2; i++) {
		ret = be_cmd_ddr_dma_test(adapter, pattern[i],
						4096, &ddrdma_cmd);
		if (ret != 0)
			goto err;
	}

err:
	pci_free_consistent(adapter->pdev, ddrdma_cmd.size,
			ddrdma_cmd.va, ddrdma_cmd.dma);
	return ret;
}

static u64 be_loopback_test(struct be_adapter *adapter, u8 loopback_type,
				u64 *status)
{
	be_cmd_set_loopback(adapter, adapter->port_num,
				loopback_type, 1);
	*status = be_cmd_loopback_test(adapter, adapter->port_num,
				loopback_type, 1500,
				2, 0xabc);
	be_cmd_set_loopback(adapter, adapter->port_num,
				BE_NO_LOOPBACK, 1);
	return *status;
}

static int
be_self_test_count(struct net_device *dev)
{
	return BE_NUM_TESTS;
}

static void
be_self_test(struct net_device *netdev, struct ethtool_test *test, u64 *data)
{
	struct be_adapter *adapter = netdev_priv(netdev);
	bool link_up;
	u8 mac_speed = 0;
	u16 qos_link_speed = 0;

	memset(data, 0, sizeof(u64) * BE_NUM_TESTS);
	if (test->flags & ETH_TEST_FL_OFFLINE) {
		if (be_loopback_test(adapter, BE_MAC_LOOPBACK,
						&data[0]) != 0) {
			test->flags |= ETH_TEST_FL_FAILED;
		}
		if (be_loopback_test(adapter, BE_PHY_LOOPBACK,
						&data[1]) != 0) {
			test->flags |= ETH_TEST_FL_FAILED;
		}
		if (be_loopback_test(adapter, BE_ONE_PORT_EXT_LOOPBACK,
						&data[2]) != 0) {
			test->flags |= ETH_TEST_FL_FAILED;
		}
	}

	if (be_test_ddr_dma(adapter) != 0) {
		data[3] = 1;
		test->flags |= ETH_TEST_FL_FAILED;
	}

	if (be_cmd_link_status_query(adapter, &link_up,
				&mac_speed, &qos_link_speed) != 0) {
		test->flags |= ETH_TEST_FL_FAILED;
		data[4] = -1;
	} else if (mac_speed) {
		data[4] = 1;
	}

}

static int
be_get_eeprom_len(struct net_device *netdev)
{
	return BE_READ_SEEPROM_LEN;
}

static int
be_read_eeprom(struct net_device *netdev, struct ethtool_eeprom *eeprom,
			uint8_t *data)
{
	struct be_adapter *adapter = netdev_priv(netdev);
	struct be_dma_mem eeprom_cmd;
	struct be_cmd_resp_seeprom_read *resp;
	int status;

	if (!eeprom->len)
		return -EINVAL;

	eeprom->magic = BE_VENDOR_ID | (adapter->pdev->device<<16);

	memset(&eeprom_cmd, 0, sizeof(struct be_dma_mem));
	eeprom_cmd.size = sizeof(struct be_cmd_req_seeprom_read);
	eeprom_cmd.va = pci_alloc_consistent(adapter->pdev, eeprom_cmd.size,
				&eeprom_cmd.dma);

	if (!eeprom_cmd.va) {
		dev_err(&adapter->pdev->dev,
			"Memory allocation failure. Could not read eeprom\n");
		return -ENOMEM;
	}

	status = be_cmd_get_seeprom_data(adapter, &eeprom_cmd);

	if (!status) {
		resp = (struct be_cmd_resp_seeprom_read *) eeprom_cmd.va;
		memcpy(data, resp->seeprom_data + eeprom->offset, eeprom->len);
	}
	pci_free_consistent(adapter->pdev, eeprom_cmd.size, eeprom_cmd.va,
			eeprom_cmd.dma);

	return status;
}

struct ethtool_ops be_ethtool_ops = {
	.get_settings = be_get_settings,
	.get_drvinfo = be_get_drvinfo,
	.get_wol = be_get_wol,
	.set_wol = be_set_wol,
	.get_link = ethtool_op_get_link,
	.get_eeprom_len = be_get_eeprom_len,
	.get_eeprom = be_read_eeprom,
	.get_coalesce = be_get_coalesce,
	.set_coalesce = be_set_coalesce,
	.get_ringparam = be_get_ringparam,
	.get_pauseparam = be_get_pauseparam,
	.set_pauseparam = be_set_pauseparam,
	.get_rx_csum = be_get_rx_csum,
	.set_rx_csum = be_set_rx_csum,
	.get_tx_csum = ethtool_op_get_tx_csum,
	.set_tx_csum = ethtool_op_set_tx_hw_csum,
	.get_sg = ethtool_op_get_sg,
	.set_sg = ethtool_op_set_sg,
	.get_tso = ethtool_op_get_tso,
	.set_tso = ethtool_op_set_tso,
	.get_strings = be_get_stat_strings,
	.phys_id = be_phys_id,
	.get_stats_count = be_get_stats_count,
	.get_ethtool_stats = be_get_ethtool_stats,
	.self_test = be_self_test,
	.self_test_count = be_self_test_count
};
