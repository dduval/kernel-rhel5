/*******************************************************************************

  Intel(R) Gigabit Ethernet Linux driver
  Copyright(c) 2007 Intel Corporation.

  This program is free software; you can redistribute it and/or modify it
  under the terms and conditions of the GNU General Public License,
  version 2, as published by the Free Software Foundation.

  This program is distributed in the hope it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
  more details.

  You should have received a copy of the GNU General Public License along with
  this program; if not, write to the Free Software Foundation, Inc.,
  51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.

  The full GNU General Public License is included in this distribution in
  the file called "COPYING".

  Contact Information:
  e1000-devel Mailing List <e1000-devel@lists.sourceforge.net>
  Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497

*******************************************************************************/

/* ethtool support for igb */

#include <linux/vmalloc.h>
#include <linux/netdevice.h>
#include <linux/ethtool.h>
#include <linux/pci.h>
#include <linux/delay.h>

#include "igb.h"
#include "igb_regtest.h"

struct igb_stats {
	char stat_string[ETH_GSTRING_LEN];
	int sizeof_stat;
	int stat_offset;
};

#define IGB_STAT(m) sizeof(((struct igb_adapter *)0)->m), \
		      offsetof(struct igb_adapter, m)
static const struct igb_stats igb_gstrings_stats[] = {
	{ "rx_packets", IGB_STAT(stats.gprc) },
	{ "tx_packets", IGB_STAT(stats.gptc) },
	{ "rx_bytes", IGB_STAT(stats.gorcl) },
	{ "tx_bytes", IGB_STAT(stats.gotcl) },
	{ "rx_broadcast", IGB_STAT(stats.bprc) },
	{ "tx_broadcast", IGB_STAT(stats.bptc) },
	{ "rx_multicast", IGB_STAT(stats.mprc) },
	{ "tx_multicast", IGB_STAT(stats.mptc) },
	{ "rx_errors", IGB_STAT(net_stats.rx_errors) },
	{ "tx_errors", IGB_STAT(net_stats.tx_errors) },
	{ "tx_dropped", IGB_STAT(net_stats.tx_dropped) },
	{ "multicast", IGB_STAT(stats.mprc) },
	{ "collisions", IGB_STAT(stats.colc) },
	{ "rx_length_errors", IGB_STAT(net_stats.rx_length_errors) },
	{ "rx_over_errors", IGB_STAT(net_stats.rx_over_errors) },
	{ "rx_crc_errors", IGB_STAT(stats.crcerrs) },
	{ "rx_frame_errors", IGB_STAT(net_stats.rx_frame_errors) },
	{ "rx_no_buffer_count", IGB_STAT(stats.rnbc) },
	{ "rx_missed_errors", IGB_STAT(stats.mpc) },
	{ "tx_aborted_errors", IGB_STAT(stats.ecol) },
	{ "tx_carrier_errors", IGB_STAT(stats.tncrs) },
	{ "tx_fifo_errors", IGB_STAT(net_stats.tx_fifo_errors) },
	{ "tx_heartbeat_errors", IGB_STAT(net_stats.tx_heartbeat_errors) },
	{ "tx_window_errors", IGB_STAT(stats.latecol) },
	{ "tx_abort_late_coll", IGB_STAT(stats.latecol) },
	{ "tx_deferred_ok", IGB_STAT(stats.dc) },
	{ "tx_single_coll_ok", IGB_STAT(stats.scc) },
	{ "tx_multi_coll_ok", IGB_STAT(stats.mcc) },
	{ "tx_timeout_count", IGB_STAT(tx_timeout_count) },
	{ "tx_restart_queue", IGB_STAT(restart_queue) },
	{ "rx_long_length_errors", IGB_STAT(stats.roc) },
	{ "rx_short_length_errors", IGB_STAT(stats.ruc) },
	{ "rx_align_errors", IGB_STAT(stats.algnerrc) },
	{ "tx_tcp_seg_good", IGB_STAT(stats.tsctc) },
	{ "tx_tcp_seg_failed", IGB_STAT(stats.tsctfc) },
	{ "rx_flow_control_xon", IGB_STAT(stats.xonrxc) },
	{ "rx_flow_control_xoff", IGB_STAT(stats.xoffrxc) },
	{ "tx_flow_control_xon", IGB_STAT(stats.xontxc) },
	{ "tx_flow_control_xoff", IGB_STAT(stats.xofftxc) },
	{ "rx_long_byte_count", IGB_STAT(stats.gorcl) },
	{ "rx_csum_offload_good", IGB_STAT(hw_csum_good) },
	{ "rx_csum_offload_errors", IGB_STAT(hw_csum_err) },
	{ "rx_header_split", IGB_STAT(rx_hdr_split) },
	{ "alloc_rx_buff_failed", IGB_STAT(alloc_rx_buff_failed) },
	{ "tx_smbus", IGB_STAT(stats.mgptc) },
	{ "rx_smbus", IGB_STAT(stats.mgprc) },
	{ "dropped_smbus", IGB_STAT(stats.mgpdc) },
};

#define IGB_QUEUE_STATS_LEN \
	((((((struct igb_adapter *)netdev->priv)->num_rx_queues > 1) ? \
	  ((struct igb_adapter *)netdev->priv)->num_rx_queues : 0 ) + \
	 (((((struct igb_adapter *)netdev->priv)->num_tx_queues > 1) ? \
	  ((struct igb_adapter *)netdev->priv)->num_tx_queues : 0 ))) * \
	(sizeof(struct igb_queue_stats) / sizeof(u64)))
#define IGB_GLOBAL_STATS_LEN	\
	sizeof(igb_gstrings_stats) / sizeof(struct igb_stats)
#define IGB_STATS_LEN (IGB_GLOBAL_STATS_LEN + IGB_QUEUE_STATS_LEN)
static const char igb_gstrings_test[][ETH_GSTRING_LEN] = {
	"Register test  (offline)", "Eeprom test    (offline)",
	"Interrupt test (offline)", "Loopback test  (offline)",
	"Link test   (on/offline)"
};
#define IGB_TEST_LEN sizeof(igb_gstrings_test) / ETH_GSTRING_LEN

static int igb_get_settings(struct net_device *netdev, struct ethtool_cmd *ecmd)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct e1000_hw *hw = &adapter->hw;

	if (hw->media_type == e1000_media_type_copper) {

		ecmd->supported = (SUPPORTED_10baseT_Half |
		                   SUPPORTED_10baseT_Full |
		                   SUPPORTED_100baseT_Half |
		                   SUPPORTED_100baseT_Full |
		                   SUPPORTED_1000baseT_Full|
		                   SUPPORTED_Autoneg |
		                   SUPPORTED_TP);
		if (hw->phy.type == e1000_phy_ife)
			ecmd->supported &= ~SUPPORTED_1000baseT_Full;
		ecmd->advertising = ADVERTISED_TP;

		if (hw->mac.autoneg == 1) {
			ecmd->advertising |= ADVERTISED_Autoneg;
			/* the e1000 autoneg seems to match ethtool nicely */
			ecmd->advertising |= hw->phy.autoneg_advertised;
		}

		ecmd->port = PORT_TP;
		ecmd->phy_address = hw->phy.addr;
	} else {
		ecmd->supported   = (SUPPORTED_1000baseT_Full |
				     SUPPORTED_FIBRE |
				     SUPPORTED_Autoneg);

		ecmd->advertising = (ADVERTISED_1000baseT_Full |
				     ADVERTISED_FIBRE |
				     ADVERTISED_Autoneg);

		ecmd->port = PORT_FIBRE;
	}

	ecmd->transceiver = XCVR_INTERNAL;

	if (E1000_READ_REG(&adapter->hw, E1000_STATUS) & E1000_STATUS_LU) {

		e1000_get_speed_and_duplex(hw, &adapter->link_speed,
		                                   &adapter->link_duplex);
		ecmd->speed = adapter->link_speed;

		/* unfortunately FULL_DUPLEX != DUPLEX_FULL
		 *          and HALF_DUPLEX != DUPLEX_HALF */

		if (adapter->link_duplex == FULL_DUPLEX)
			ecmd->duplex = DUPLEX_FULL;
		else
			ecmd->duplex = DUPLEX_HALF;
	} else {
		ecmd->speed = -1;
		ecmd->duplex = -1;
	}

	ecmd->autoneg = ((hw->media_type == e1000_media_type_fiber) ||
			 hw->mac.autoneg) ? AUTONEG_ENABLE : AUTONEG_DISABLE;
	return 0;
}

static int igb_set_settings(struct net_device *netdev, struct ethtool_cmd *ecmd)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct e1000_hw *hw = &adapter->hw;

	/* When SoL/IDER sessions are active, autoneg/speed/duplex
	 * cannot be changed */
	if (e1000_check_reset_block(hw)) {
		DPRINTK(DRV, ERR, "Cannot change link characteristics "
		        "when SoL/IDER is active.\n");
		return -EINVAL;
	}

	while (test_and_set_bit(__IGB_RESETTING, &adapter->state))
		msleep(1);

	if (ecmd->autoneg == AUTONEG_ENABLE) {
		hw->mac.autoneg = 1;
		if (hw->media_type == e1000_media_type_fiber)
			hw->phy.autoneg_advertised = ADVERTISED_1000baseT_Full |
			                             ADVERTISED_FIBRE |
			                             ADVERTISED_Autoneg;
		else
			hw->phy.autoneg_advertised = ecmd->advertising |
			                             ADVERTISED_TP |
			                             ADVERTISED_Autoneg;
		ecmd->advertising = hw->phy.autoneg_advertised;
	} else
		if (igb_set_spd_dplx(adapter, ecmd->speed + ecmd->duplex)) {
			clear_bit(__IGB_RESETTING, &adapter->state);
			return -EINVAL;
		}

	/* reset the link */

	if (netif_running(adapter->netdev)) {
		igb_down(adapter);
		igb_up(adapter);
	} else
		igb_reset(adapter);

	clear_bit(__IGB_RESETTING, &adapter->state);
	return 0;
}

static void igb_get_pauseparam(struct net_device *netdev,
                               struct ethtool_pauseparam *pause)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct e1000_hw *hw = &adapter->hw;

	pause->autoneg =
		(adapter->fc_autoneg ? AUTONEG_ENABLE : AUTONEG_DISABLE);

	if (hw->mac.fc == e1000_fc_rx_pause)
		pause->rx_pause = 1;
	else if (hw->mac.fc == e1000_fc_tx_pause)
		pause->tx_pause = 1;
	else if (hw->mac.fc == e1000_fc_full) {
		pause->rx_pause = 1;
		pause->tx_pause = 1;
	}
}

static int igb_set_pauseparam(struct net_device *netdev,
                              struct ethtool_pauseparam *pause)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct e1000_hw *hw = &adapter->hw;
	int retval = 0;

	adapter->fc_autoneg = pause->autoneg;

	while (test_and_set_bit(__IGB_RESETTING, &adapter->state))
		msleep(1);

	if (pause->rx_pause && pause->tx_pause)
		hw->mac.fc = e1000_fc_full;
	else if (pause->rx_pause && !pause->tx_pause)
		hw->mac.fc = e1000_fc_rx_pause;
	else if (!pause->rx_pause && pause->tx_pause)
		hw->mac.fc = e1000_fc_tx_pause;
	else if (!pause->rx_pause && !pause->tx_pause)
		hw->mac.fc = e1000_fc_none;

	hw->mac.original_fc = hw->mac.fc;

	if (adapter->fc_autoneg == AUTONEG_ENABLE) {
		if (netif_running(adapter->netdev)) {
			igb_down(adapter);
			igb_up(adapter);
		} else
			igb_reset(adapter);
	} else
		retval = ((hw->media_type == e1000_media_type_fiber) ?
			  e1000_setup_link(hw) : e1000_force_mac_fc(hw));

	clear_bit(__IGB_RESETTING, &adapter->state);
	return retval;
}

static u32 igb_get_rx_csum(struct net_device *netdev)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	return adapter->rx_csum;
}

static int igb_set_rx_csum(struct net_device *netdev, u32 data)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	adapter->rx_csum = data;

	return 0;
}

static u32 igb_get_tx_csum(struct net_device *netdev)
{
	return (netdev->features & NETIF_F_HW_CSUM) != 0;
}

static int igb_set_tx_csum(struct net_device *netdev, u32 data)
{
	if (data)
		netdev->features |= NETIF_F_HW_CSUM;
	else
		netdev->features &= ~NETIF_F_HW_CSUM;

	return 0;
}

static int igb_set_tso(struct net_device *netdev, u32 data)
{
	struct igb_adapter *adapter = netdev_priv(netdev);

	if (data)
		netdev->features |= NETIF_F_TSO;
	else
		netdev->features &= ~NETIF_F_TSO;

	if (data)
		netdev->features |= NETIF_F_TSO6;
	else
		netdev->features &= ~NETIF_F_TSO6;

	DPRINTK(PROBE, INFO, "TSO is %s\n", data ? "Enabled" : "Disabled");
	return 0;
}

static u32 igb_get_msglevel(struct net_device *netdev)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	return adapter->msg_enable;
}

static void igb_set_msglevel(struct net_device *netdev, u32 data)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	adapter->msg_enable = data;
}

static int igb_get_regs_len(struct net_device *netdev)
{
#define IGB_REGS_LEN 32
	return IGB_REGS_LEN * sizeof(u32);
}

static void igb_get_regs(struct net_device *netdev,
	                 struct ethtool_regs *regs, void *p)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct e1000_hw *hw = &adapter->hw;
	u32 *regs_buff = p;
	u16 phy_data;

	memset(p, 0, IGB_REGS_LEN * sizeof(u32));

	regs->version = (1 << 24) | (hw->revision_id << 16) | hw->device_id;

	regs_buff[0]  = E1000_READ_REG(hw, E1000_CTRL);
	regs_buff[1]  = E1000_READ_REG(hw, E1000_STATUS);

	regs_buff[2]  = E1000_READ_REG(hw, E1000_RCTL);
	regs_buff[3]  = E1000_READ_REG(hw, E1000_RDLEN);
	regs_buff[4]  = E1000_READ_REG(hw, E1000_RDH);
	regs_buff[5]  = E1000_READ_REG(hw, E1000_RDT);
	regs_buff[6]  = E1000_READ_REG(hw, E1000_RDTR);

	regs_buff[7]  = E1000_READ_REG(hw, E1000_TCTL);
	regs_buff[8]  = E1000_READ_REG(hw, E1000_TDLEN);
	regs_buff[9]  = E1000_READ_REG(hw, E1000_TDH);
	regs_buff[10] = E1000_READ_REG(hw, E1000_TDT);
	regs_buff[11] = E1000_READ_REG(hw, E1000_TIDV);

	regs_buff[12] = adapter->hw.phy.type;  /* PHY type (IGP=1, M88=0) */
	e1000_read_phy_reg(hw, M88E1000_PHY_SPEC_STATUS, &phy_data);
	regs_buff[13] = (u32)phy_data; /* cable length */
	regs_buff[14] = 0;  /* Dummy (to align w/ IGP phy reg dump) */
	regs_buff[15] = 0;  /* Dummy (to align w/ IGP phy reg dump) */
	regs_buff[16] = 0;  /* Dummy (to align w/ IGP phy reg dump) */
	e1000_read_phy_reg(hw, M88E1000_PHY_SPEC_CTRL, &phy_data);
	regs_buff[17] = (u32)phy_data; /* extended 10bt distance */
	regs_buff[18] = regs_buff[13]; /* cable polarity */
	regs_buff[19] = 0;  /* Dummy (to align w/ IGP phy reg dump) */
	regs_buff[20] = regs_buff[17]; /* polarity correction */
	/* phy receive errors */
	regs_buff[22] = adapter->phy_stats.receive_errors;
	regs_buff[23] = regs_buff[13]; /* mdix mode */
	regs_buff[21] = adapter->phy_stats.idle_errors;  /* phy idle errors */
	e1000_read_phy_reg(hw, PHY_1000T_STATUS, &phy_data);
	regs_buff[24] = (u32)phy_data;  /* phy local receiver status */
	regs_buff[25] = regs_buff[24];  /* phy remote receiver status */
}

static int igb_get_eeprom_len(struct net_device *netdev)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	return adapter->hw.nvm.word_size * 2;
}

static int igb_get_eeprom(struct net_device *netdev,
                          struct ethtool_eeprom *eeprom, u8 *bytes)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct e1000_hw *hw = &adapter->hw;
	u16 *eeprom_buff;
	int first_word, last_word;
	int ret_val = 0;
	u16 i;

	if (eeprom->len == 0)
		return -EINVAL;

	eeprom->magic = hw->vendor_id | (hw->device_id << 16);

	first_word = eeprom->offset >> 1;
	last_word = (eeprom->offset + eeprom->len - 1) >> 1;

	eeprom_buff = kmalloc(sizeof(u16) *
			(last_word - first_word + 1), GFP_KERNEL);
	if (!eeprom_buff)
		return -ENOMEM;

	if (hw->nvm.type == e1000_nvm_eeprom_spi)
		ret_val = e1000_read_nvm(hw, first_word,
		                         last_word - first_word + 1,
		                         eeprom_buff);
	else {
		for (i = 0; i < last_word - first_word + 1; i++)
			if ((ret_val = e1000_read_nvm(hw, first_word + i, 1,
			                              &eeprom_buff[i])))
				break;
	}

	/* Device's eeprom is always little-endian, word addressable */
	for (i = 0; i < last_word - first_word + 1; i++)
		le16_to_cpus(&eeprom_buff[i]);

	memcpy(bytes, (u8 *)eeprom_buff + (eeprom->offset & 1),
			eeprom->len);
	kfree(eeprom_buff);

	return ret_val;
}

static int igb_set_eeprom(struct net_device *netdev,
                          struct ethtool_eeprom *eeprom, u8 *bytes)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct e1000_hw *hw = &adapter->hw;
	u16 *eeprom_buff;
	void *ptr;
	int max_len, first_word, last_word, ret_val = 0;
	u16 i;

	if (eeprom->len == 0)
		return -EOPNOTSUPP;

	if (eeprom->magic != (hw->vendor_id | (hw->device_id << 16)))
		return -EFAULT;

	max_len = hw->nvm.word_size * 2;

	first_word = eeprom->offset >> 1;
	last_word = (eeprom->offset + eeprom->len - 1) >> 1;
	eeprom_buff = kmalloc(max_len, GFP_KERNEL);
	if (!eeprom_buff)
		return -ENOMEM;

	ptr = (void *)eeprom_buff;

	if (eeprom->offset & 1) {
		/* need read/modify/write of first changed EEPROM word */
		/* only the second byte of the word is being modified */
		ret_val = e1000_read_nvm(hw, first_word, 1,
					    &eeprom_buff[0]);
		ptr++;
	}
	if (((eeprom->offset + eeprom->len) & 1) && (ret_val == 0)) {
		/* need read/modify/write of last changed EEPROM word */
		/* only the first byte of the word is being modified */
		ret_val = e1000_read_nvm(hw, last_word, 1,
		                  &eeprom_buff[last_word - first_word]);
	}

	/* Device's eeprom is always little-endian, word addressable */
	for (i = 0; i < last_word - first_word + 1; i++)
		le16_to_cpus(&eeprom_buff[i]);

	memcpy(ptr, bytes, eeprom->len);

	for (i = 0; i < last_word - first_word + 1; i++)
		eeprom_buff[i] = cpu_to_le16(eeprom_buff[i]);

	ret_val = e1000_write_nvm(hw, first_word,
	                          last_word - first_word + 1, eeprom_buff);

	/* Update the checksum over the first part of the EEPROM if needed
	 * and flush shadow RAM for 82573 controllers */
	if ((ret_val == 0) && ((first_word <= NVM_CHECKSUM_REG)))
		e1000_update_nvm_checksum(hw);

	kfree(eeprom_buff);
	return ret_val;
}

static void igb_get_drvinfo(struct net_device *netdev,
                            struct ethtool_drvinfo *drvinfo)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	char firmware_version[32];
	u16 eeprom_data;

	strncpy(drvinfo->driver,  igb_driver_name, 32);
	strncpy(drvinfo->version, igb_driver_version, 32);

	/* EEPROM image version # is reported as firmware version # for
	 * 82575 controllers */
	e1000_read_nvm(&adapter->hw, 5, 1, &eeprom_data);
	sprintf(firmware_version, "%d.%d-%d",
		(eeprom_data & 0xF000) >> 12,
		(eeprom_data & 0x0FF0) >> 4,
		eeprom_data & 0x000F);

	strncpy(drvinfo->fw_version, firmware_version, 32);
	strncpy(drvinfo->bus_info, pci_name(adapter->pdev), 32);
	drvinfo->n_stats = IGB_STATS_LEN;
	drvinfo->testinfo_len = IGB_TEST_LEN;
	drvinfo->regdump_len = igb_get_regs_len(netdev);
	drvinfo->eedump_len = igb_get_eeprom_len(netdev);
}

static void igb_get_ringparam(struct net_device *netdev,
                              struct ethtool_ringparam *ring)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct igb_ring *tx_ring = adapter->tx_ring;
	struct igb_ring *rx_ring = adapter->rx_ring;

	ring->rx_max_pending = IGB_MAX_RXD;
	ring->tx_max_pending = IGB_MAX_TXD;
	ring->rx_mini_max_pending = 0;
	ring->rx_jumbo_max_pending = 0;
	ring->rx_pending = rx_ring->count;
	ring->tx_pending = tx_ring->count;
	ring->rx_mini_pending = 0;
	ring->rx_jumbo_pending = 0;
}

static int igb_set_ringparam(struct net_device *netdev,
                             struct ethtool_ringparam *ring)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct igb_buffer *old_buf;
	struct igb_buffer *old_rx_buf;
	void *old_desc;
	int i, err;
	u32 new_rx_count, new_tx_count, old_size;
	dma_addr_t old_dma;

	if ((ring->rx_mini_pending) || (ring->rx_jumbo_pending))
		return -EINVAL;

	new_rx_count = max(ring->rx_pending, (u32)IGB_MIN_RXD);
	new_rx_count = min(new_rx_count, (u32) IGB_MAX_RXD);
	new_rx_count = ALIGN(new_rx_count, REQ_RX_DESCRIPTOR_MULTIPLE);

	new_tx_count = max(ring->tx_pending, (u32)IGB_MIN_TXD);
	new_tx_count = min(new_tx_count, (u32) IGB_MAX_TXD);
	new_tx_count = ALIGN(new_tx_count, REQ_TX_DESCRIPTOR_MULTIPLE);

	if ((new_tx_count == adapter->tx_ring->count) &&
	    (new_rx_count == adapter->rx_ring->count)) {
		/* nothing to do */
		return 0;
	}

	while (test_and_set_bit(__IGB_RESETTING, &adapter->state))
		msleep(1);

	if (netif_running(adapter->netdev))
		igb_down(adapter);

	/*
	 * We can't just free everything and then setup again,
	 * because the ISRs in MSI-X mode get passed pointers
	 * to the tx and rx ring structs.
	 */
	if (new_tx_count != adapter->tx_ring->count) {
		for (i = 0; i < adapter->num_tx_queues; i++) {
			/* Save existing descriptor ring */
			old_buf = adapter->tx_ring[i].buffer_info;
			old_desc = adapter->tx_ring[i].desc;
			old_size = adapter->tx_ring[i].size;
			old_dma = adapter->tx_ring[i].dma;
			/* Try to allocate a new one */
			adapter->tx_ring[i].buffer_info = NULL;
			adapter->tx_ring[i].desc = NULL;
			adapter->tx_ring[i].count = new_tx_count;
			err = igb_setup_tx_resources(adapter,
						&adapter->tx_ring[i]);
			if (err) {
				/* Restore the old one so at least
				   the adapter still works, even if
				   we failed the request */
				adapter->tx_ring[i].buffer_info = old_buf;
				adapter->tx_ring[i].desc = old_desc;
				adapter->tx_ring[i].size = old_size;
				adapter->tx_ring[i].dma = old_dma;
				goto err_setup;
			}
			/* Free the old buffer manually */
			vfree(old_buf);
			pci_free_consistent(adapter->pdev, old_size,
			                    old_desc, old_dma);
		}
	}

	if (new_rx_count != adapter->rx_ring->count) {
		for (i = 0; i < adapter->num_rx_queues; i++) {

			old_rx_buf = adapter->rx_ring[i].buffer_info;
			old_desc = adapter->rx_ring[i].desc;
			old_size = adapter->rx_ring[i].size;
			old_dma = adapter->rx_ring[i].dma;

			adapter->rx_ring[i].buffer_info = NULL;
			adapter->rx_ring[i].desc = NULL;
			adapter->rx_ring[i].dma = 0;
			adapter->rx_ring[i].count = new_rx_count;
			err = igb_setup_rx_resources(adapter,
			                             &adapter->rx_ring[i]);
			if (err) {
				adapter->rx_ring[i].buffer_info = old_rx_buf;
				adapter->rx_ring[i].desc = old_desc;
				adapter->rx_ring[i].size = old_size;
				adapter->rx_ring[i].dma = old_dma;
				goto err_setup;
			}

			vfree(old_rx_buf);
			pci_free_consistent(adapter->pdev, old_size, old_desc,
			                    old_dma);
		}
	}

	err = 0;
err_setup:
	if (netif_running(adapter->netdev))
		igb_up(adapter);

	clear_bit(__IGB_RESETTING, &adapter->state);
	return err;
}

#define REG_PATTERN_TEST(R, M, W)                                              \
{                                                                              \
	u32 _pat, _value;                                                      \
	u32 _test[] =                                                          \
		{0x5A5A5A5A, 0xA5A5A5A5, 0x00000000, 0xFFFFFFFF};              \
	for (_pat = 0; _pat < ARRAY_SIZE(_test); _pat++) {                     \
		writel((_test[_pat] & W), (adapter->hw.hw_addr + R));          \
		_value = readl(adapter->hw.hw_addr + R);                       \
		if (_value != (_test[_pat] & W & M)) {                         \
			DPRINTK(DRV, ERR, "pattern test reg %04X failed: got " \
			        "0x%08X expected 0x%08X\n",                    \
			        R, _value, (_test[_pat] & W & M));             \
			*data = R;                                             \
			return 1;                                              \
		}                                                              \
	}                                                                      \
}

#define REG_SET_AND_CHECK(R, M, W)                                             \
{                                                                              \
	u32 _value;                                                             \
	writel((W & M), (adapter->hw.hw_addr + R));                            \
	_value = readl(adapter->hw.hw_addr + R);                               \
	if ((W & M) != (_value & M)) {                                         \
		DPRINTK(DRV, ERR, "set/check reg %04X test failed: got 0x%08X "\
		        "expected 0x%08X\n", R, (_value & M), (W & M));        \
		*data = R;                                                     \
		return 1;                                                      \
	}                                                                      \
}

static int igb_reg_test(struct igb_adapter *adapter, u64 *data)
{
	struct igb_reg_test *test;
	u32 value, before, after;
	u32 i, toggle;

	toggle = 0x7FFFF3FF;
	test = reg_test_82575;

	/* Because the status register is such a special case,
	 * we handle it separately from the rest of the register
	 * tests.  Some bits are read-only, some toggle, and some
	 * are writable on newer MACs.
	 */
	before = E1000_READ_REG(&adapter->hw, E1000_STATUS);
	value = (E1000_READ_REG(&adapter->hw, E1000_STATUS) & toggle);
	E1000_WRITE_REG(&adapter->hw, E1000_STATUS, toggle);
	after = E1000_READ_REG(&adapter->hw, E1000_STATUS) & toggle;
	if (value != after) {
		DPRINTK(DRV, ERR, "failed STATUS register test got: "
		        "0x%08X expected: 0x%08X\n", after, value);
		*data = 1;
		return 1;
	}
	/* restore previous status */
	E1000_WRITE_REG(&adapter->hw, E1000_STATUS, before);

	/* Perform the remainder of the register test, looping through
	 * the test table until we either fail or reach the null entry.
	 */
	while (test->reg) {
		for (i = 0; i < test->array_len; i++) {
			switch (test->test_type) {
			case PATTERN_TEST:
				REG_PATTERN_TEST(test->reg + (i * 0x100),
						test->mask,
						test->write);
				break;
			case SET_READ_TEST:
				REG_SET_AND_CHECK(test->reg + (i * 0x100),
						test->mask,
						test->write);
				break;
			case WRITE_NO_TEST:
				writel(test->write,
				    (adapter->hw.hw_addr + test->reg)
				        + (i * 0x100));
				break;
			case TABLE32_TEST:
				REG_PATTERN_TEST(test->reg + (i * 4),
						test->mask,
						test->write);
				break;
			case TABLE64_TEST_LO:
				REG_PATTERN_TEST(test->reg + (i * 8),
						test->mask,
						test->write);
				break;
			case TABLE64_TEST_HI:
				REG_PATTERN_TEST((test->reg + 4) + (i * 8),
						test->mask,
						test->write);
				break;
			}
		}
		test++;
	}

	*data = 0;
	return 0;
}

static int igb_eeprom_test(struct igb_adapter *adapter, u64 *data)
{
	u16 temp;
	u16 checksum = 0;
	u16 i;

	*data = 0;
	/* Read and add up the contents of the EEPROM */
	for (i = 0; i < (NVM_CHECKSUM_REG + 1); i++) {
		if ((e1000_read_nvm(&adapter->hw, i, 1, &temp)) < 0) {
			*data = 1;
			break;
		}
		checksum += temp;
	}

	/* If Checksum is not Correct return error else test passed */
	if ((checksum != (u16) NVM_SUM) && !(*data))
		*data = 2;

	return *data;
}

static irqreturn_t igb_test_intr(int irq, void *data, struct pt_regs *regs)
{
	struct net_device *netdev = (struct net_device *) data;
	struct igb_adapter *adapter = netdev_priv(netdev);

	adapter->test_icr |= E1000_READ_REG(&adapter->hw, E1000_ICR);

	return IRQ_HANDLED;
}

static int igb_intr_test(struct igb_adapter *adapter, u64 *data)
{
	struct net_device *netdev = adapter->netdev;
	u32 mask, i = 0, shared_int = 1;
	u32 irq = adapter->pdev->irq;

	*data = 0;

	/* Hook up test interrupt handler just for this test */
	if (adapter->msix_entries) {
		/* NOTE: we don't test MSI-X interrupts here, yet */
		return 0;
	} else if (adapter->flags & FLAG_IGB_HAS_MSI) {
		shared_int = 0;
		if (request_irq(irq, &igb_test_intr, 0, netdev->name, netdev)) {
			*data = 1;
			return -1;
		}
	} else if (!request_irq(irq, &igb_test_intr, IRQF_PROBE_SHARED,
	                        netdev->name, netdev)) {
		shared_int = 0;
	} else if (request_irq(irq, &igb_test_intr, IRQF_SHARED,
	                       netdev->name, netdev)) {
		*data = 1;
		return -1;
	}
	DPRINTK(HW, INFO, "testing %s interrupt\n",
	        (shared_int ? "shared" : "unshared"));

	/* Disable all the interrupts */
	E1000_WRITE_REG(&adapter->hw, E1000_IMC, 0xFFFFFFFF);
	msleep(10);

	/* Test each interrupt */
	for (; i < 10; i++) {
		/* Interrupt to test */
		mask = 1 << i;

		if (!shared_int) {
			/* Disable the interrupt to be reported in
			 * the cause register and then force the same
			 * interrupt and see if one gets posted.  If
			 * an interrupt was posted to the bus, the
			 * test failed.
			 */
			adapter->test_icr = 0;
			E1000_WRITE_REG(&adapter->hw, E1000_IMC,
			                ~mask & 0x00007FFF);
			E1000_WRITE_REG(&adapter->hw, E1000_ICS,
			                ~mask & 0x00007FFF);
			msleep(10);

			if (adapter->test_icr & mask) {
				*data = 3;
				break;
			}
		}

		/* Enable the interrupt to be reported in
		 * the cause register and then force the same
		 * interrupt and see if one gets posted.  If
		 * an interrupt was not posted to the bus, the
		 * test failed.
		 */
		adapter->test_icr = 0;
		E1000_WRITE_REG(&adapter->hw, E1000_IMS, mask);
		E1000_WRITE_REG(&adapter->hw, E1000_ICS, mask);
		msleep(10);

		if (!(adapter->test_icr & mask)) {
			*data = 4;
			break;
		}

		if (!shared_int) {
			/* Disable the other interrupts to be reported in
			 * the cause register and then force the other
			 * interrupts and see if any get posted.  If
			 * an interrupt was posted to the bus, the
			 * test failed.
			 */
			adapter->test_icr = 0;
			E1000_WRITE_REG(&adapter->hw, E1000_IMC,
			                ~mask & 0x00007FFF);
			E1000_WRITE_REG(&adapter->hw, E1000_ICS,
			                ~mask & 0x00007FFF);
			msleep(10);

			if (adapter->test_icr) {
				*data = 5;
				break;
			}
		}
	}

	/* Disable all the interrupts */
	E1000_WRITE_REG(&adapter->hw, E1000_IMC, 0xFFFFFFFF);
	msleep(10);

	/* Unhook test interrupt handler */
	free_irq(irq, netdev);

	return *data;
}

static void igb_free_desc_rings(struct igb_adapter *adapter)
{
	struct igb_ring *tx_ring = &adapter->test_tx_ring;
	struct igb_ring *rx_ring = &adapter->test_rx_ring;
	struct pci_dev *pdev = adapter->pdev;
	int i;

	if (tx_ring->desc && tx_ring->buffer_info) {
		for (i = 0; i < tx_ring->count; i++) {
			struct igb_buffer *buf = &(tx_ring->buffer_info[i]);
			if (buf->dma)
				pci_unmap_single(pdev, buf->dma, buf->length,
				                 PCI_DMA_TODEVICE);
			if (buf->skb)
				dev_kfree_skb(buf->skb);
		}
	}

	if (rx_ring->desc && rx_ring->buffer_info) {
		for (i = 0; i < rx_ring->count; i++) {
			struct igb_buffer *buf = &(rx_ring->buffer_info[i]);
			if (buf->dma)
				pci_unmap_single(pdev, buf->dma,
				                 IGB_RXBUFFER_2048,
				                 PCI_DMA_FROMDEVICE);
			if (buf->skb)
				dev_kfree_skb(buf->skb);
		}
	}

	if (tx_ring->desc) {
		pci_free_consistent(pdev, tx_ring->size, tx_ring->desc,
		                    tx_ring->dma);
		tx_ring->desc = NULL;
	}
	if (rx_ring->desc) {
		pci_free_consistent(pdev, rx_ring->size, rx_ring->desc,
		                    rx_ring->dma);
		rx_ring->desc = NULL;
	}

	kfree(tx_ring->buffer_info);
	tx_ring->buffer_info = NULL;
	kfree(rx_ring->buffer_info);
	rx_ring->buffer_info = NULL;

	return;
}

static int igb_setup_desc_rings(struct igb_adapter *adapter)
{
	struct igb_ring *tx_ring = &adapter->test_tx_ring;
	struct igb_ring *rx_ring = &adapter->test_rx_ring;
	struct pci_dev *pdev = adapter->pdev;
	u32 rctl;
	int size, i, ret_val;

	/* Setup Tx descriptor ring and Tx buffers */

	if (!tx_ring->count)
		tx_ring->count = IGB_DEFAULT_TXD;

	size = tx_ring->count * sizeof(struct igb_buffer);
	tx_ring->buffer_info = kzalloc(size, GFP_KERNEL);
	if (!tx_ring->buffer_info) {
		ret_val = 1;
		goto err_nomem;
	}

	tx_ring->size = tx_ring->count * sizeof(struct e1000_tx_desc);
	tx_ring->size = ALIGN(tx_ring->size, 4096);
	tx_ring->desc = pci_alloc_consistent(pdev, tx_ring->size,
	                                     &tx_ring->dma);
	if (!tx_ring->desc) {
		ret_val = 2;
		goto err_nomem;
	}
	tx_ring->next_to_use = tx_ring->next_to_clean = 0;

	E1000_WRITE_REG(&adapter->hw, E1000_TDBAL,
			((u64) tx_ring->dma & 0x00000000FFFFFFFF));
	E1000_WRITE_REG(&adapter->hw, E1000_TDBAH, ((u64) tx_ring->dma >> 32));
	E1000_WRITE_REG(&adapter->hw, E1000_TDLEN,
			tx_ring->count * sizeof(struct e1000_tx_desc));
	E1000_WRITE_REG(&adapter->hw, E1000_TDH, 0);
	E1000_WRITE_REG(&adapter->hw, E1000_TDT, 0);
	E1000_WRITE_REG(&adapter->hw, E1000_TCTL,
			E1000_TCTL_PSP | E1000_TCTL_EN |
			E1000_COLLISION_THRESHOLD << E1000_CT_SHIFT |
			E1000_COLLISION_DISTANCE << E1000_COLD_SHIFT);

	for (i = 0; i < tx_ring->count; i++) {
		struct e1000_tx_desc *tx_desc = E1000_TX_DESC(*tx_ring, i);
		struct sk_buff *skb;
		unsigned int skb_size = 1024;

		skb = alloc_skb(skb_size, GFP_KERNEL);
		if (!skb) {
			ret_val = 3;
			goto err_nomem;
		}
		skb_put(skb, skb_size);
		tx_ring->buffer_info[i].skb = skb;
		tx_ring->buffer_info[i].length = skb->len;
		tx_ring->buffer_info[i].dma =
			pci_map_single(pdev, skb->data, skb->len,
				       PCI_DMA_TODEVICE);
		tx_desc->buffer_addr = cpu_to_le64(tx_ring->buffer_info[i].dma);
		tx_desc->lower.data = cpu_to_le32(skb->len);
		tx_desc->lower.data |= cpu_to_le32(E1000_TXD_CMD_EOP |
						   E1000_TXD_CMD_IFCS |
						   E1000_TXD_CMD_RS);
		tx_desc->upper.data = 0;
	}

	/* Setup Rx descriptor ring and Rx buffers */

	if (!rx_ring->count)
		rx_ring->count = IGB_DEFAULT_RXD;

	size = rx_ring->count * sizeof(struct igb_buffer);
	rx_ring->buffer_info = kzalloc(size, GFP_KERNEL);
	if (!rx_ring->buffer_info) {
		ret_val = 4;
		goto err_nomem;
	}

	rx_ring->size = rx_ring->count * sizeof(struct e1000_rx_desc);
	rx_ring->desc = pci_alloc_consistent(pdev, rx_ring->size,
	                                     &rx_ring->dma);
	if (!rx_ring->desc) {
		ret_val = 5;
		goto err_nomem;
	}
	rx_ring->next_to_use = rx_ring->next_to_clean = 0;

	rctl = E1000_READ_REG(&adapter->hw, E1000_RCTL);
	E1000_WRITE_REG(&adapter->hw, E1000_RCTL, rctl & ~E1000_RCTL_EN);
	E1000_WRITE_REG(&adapter->hw, E1000_RDBAL,
			((u64) rx_ring->dma & 0xFFFFFFFF));
	E1000_WRITE_REG(&adapter->hw, E1000_RDBAH, ((u64) rx_ring->dma >> 32));
	E1000_WRITE_REG(&adapter->hw, E1000_RDLEN, rx_ring->size);
	E1000_WRITE_REG(&adapter->hw, E1000_RDH, 0);
	E1000_WRITE_REG(&adapter->hw, E1000_RDT, 0);
	rctl = E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SZ_2048 |
		E1000_RCTL_LBM_NO | E1000_RCTL_RDMTS_HALF |
		(adapter->hw.mac.mc_filter_type << E1000_RCTL_MO_SHIFT);
	E1000_WRITE_REG(&adapter->hw, E1000_RCTL, rctl);
	E1000_WRITE_REG(&adapter->hw, E1000_SRRCTL0, 0);

	for (i = 0; i < rx_ring->count; i++) {
		struct e1000_rx_desc *rx_desc = E1000_RX_DESC(*rx_ring, i);
		struct sk_buff *skb;

		skb = alloc_skb(IGB_RXBUFFER_2048 + NET_IP_ALIGN, GFP_KERNEL);
		if (!skb) {
			ret_val = 6;
			goto err_nomem;
		}
		skb_reserve(skb, NET_IP_ALIGN);
		rx_ring->buffer_info[i].skb = skb;
		rx_ring->buffer_info[i].dma =
			pci_map_single(pdev, skb->data, IGB_RXBUFFER_2048,
				       PCI_DMA_FROMDEVICE);
		rx_desc->buffer_addr = cpu_to_le64(rx_ring->buffer_info[i].dma);
		memset(skb->data, 0x00, skb->len);
	}

	return 0;

err_nomem:
	igb_free_desc_rings(adapter);
	return ret_val;
}

static void igb_phy_disable_receiver(struct igb_adapter *adapter)
{
	/* Write out to PHY registers 29 and 30 to disable the Receiver. */
	e1000_write_phy_reg(&adapter->hw, 29, 0x001F);
	e1000_write_phy_reg(&adapter->hw, 30, 0x8FFC);
	e1000_write_phy_reg(&adapter->hw, 29, 0x001A);
	e1000_write_phy_reg(&adapter->hw, 30, 0x8FF0);
}

static int igb_integrated_phy_loopback(struct igb_adapter *adapter)
{
	u32 ctrl_reg = 0;
	u32 stat_reg = 0;

	adapter->hw.mac.autoneg = 0;

	if (adapter->hw.phy.type == e1000_phy_m88) {
		/* Auto-MDI/MDIX Off */
		e1000_write_phy_reg(&adapter->hw,
				    M88E1000_PHY_SPEC_CTRL, 0x0808);
		/* reset to update Auto-MDI/MDIX */
		e1000_write_phy_reg(&adapter->hw, PHY_CONTROL, 0x9140);
		/* autoneg off */
		e1000_write_phy_reg(&adapter->hw, PHY_CONTROL, 0x8140);
	} else if (adapter->hw.phy.type == e1000_phy_gg82563)
		e1000_write_phy_reg(&adapter->hw,
		                    GG82563_PHY_KMRN_MODE_CTRL,
		                    0x1CC);

	ctrl_reg = E1000_READ_REG(&adapter->hw, E1000_CTRL);

	if (adapter->hw.phy.type == e1000_phy_ife) {
		/* force 100, set loopback */
		e1000_write_phy_reg(&adapter->hw, PHY_CONTROL, 0x6100);

		/* Now set up the MAC to the same speed/duplex as the PHY. */
		ctrl_reg &= ~E1000_CTRL_SPD_SEL; /* Clear the speed sel bits */
		ctrl_reg |= (E1000_CTRL_FRCSPD | /* Set the Force Speed Bit */
			     E1000_CTRL_FRCDPX | /* Set the Force Duplex Bit */
			     E1000_CTRL_SPD_100 |/* Force Speed to 100 */
			     E1000_CTRL_FD);	 /* Force Duplex to FULL */
	} else {
		/* force 1000, set loopback */
		e1000_write_phy_reg(&adapter->hw, PHY_CONTROL, 0x4140);

		/* Now set up the MAC to the same speed/duplex as the PHY. */
		ctrl_reg = E1000_READ_REG(&adapter->hw, E1000_CTRL);
		ctrl_reg &= ~E1000_CTRL_SPD_SEL; /* Clear the speed sel bits */
		ctrl_reg |= (E1000_CTRL_FRCSPD | /* Set the Force Speed Bit */
			     E1000_CTRL_FRCDPX | /* Set the Force Duplex Bit */
			     E1000_CTRL_SPD_1000 |/* Force Speed to 1000 */
			     E1000_CTRL_FD);	 /* Force Duplex to FULL */
	}

	if (adapter->hw.media_type == e1000_media_type_copper &&
	    adapter->hw.phy.type == e1000_phy_m88)
		ctrl_reg |= E1000_CTRL_ILOS; /* Invert Loss of Signal */
	else {
		/* Set the ILOS bit on the fiber Nic if half duplex link is
		 * detected. */
		stat_reg = E1000_READ_REG(&adapter->hw, E1000_STATUS);
		if ((stat_reg & E1000_STATUS_FD) == 0)
			ctrl_reg |= (E1000_CTRL_ILOS | E1000_CTRL_SLU);
	}

	E1000_WRITE_REG(&adapter->hw, E1000_CTRL, ctrl_reg);

	/* Disable the receiver on the PHY so when a cable is plugged in, the
	 * PHY does not begin to autoneg when a cable is reconnected to the NIC.
	 */
	if (adapter->hw.phy.type == e1000_phy_m88)
		igb_phy_disable_receiver(adapter);

	udelay(500);

	return 0;
}

static int igb_set_phy_loopback(struct igb_adapter *adapter)
{
	return igb_integrated_phy_loopback(adapter);
}

static int igb_setup_loopback_test(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 rctl;

	if (hw->media_type == e1000_media_type_fiber ||
	    hw->media_type == e1000_media_type_internal_serdes) {
		rctl = E1000_READ_REG(hw, E1000_RCTL);
		rctl |= E1000_RCTL_LBM_TCVR;
		E1000_WRITE_REG(hw, E1000_RCTL, rctl);
		return 0;
	} else if (hw->media_type == e1000_media_type_copper) {
		return igb_set_phy_loopback(adapter);
	}

	return 7;
}

static void igb_loopback_cleanup(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 rctl;
	u16 phy_reg;

	rctl = E1000_READ_REG(hw, E1000_RCTL);
	rctl &= ~(E1000_RCTL_LBM_TCVR | E1000_RCTL_LBM_MAC);
	E1000_WRITE_REG(hw, E1000_RCTL, rctl);

	hw->mac.autoneg = 1;
	if (hw->phy.type == e1000_phy_gg82563)
		e1000_write_phy_reg(hw, GG82563_PHY_KMRN_MODE_CTRL, 0x180);
	e1000_read_phy_reg(hw, PHY_CONTROL, &phy_reg);
	if (phy_reg & MII_CR_LOOPBACK) {
		phy_reg &= ~MII_CR_LOOPBACK;
		e1000_write_phy_reg(hw, PHY_CONTROL, phy_reg);
		e1000_phy_commit(hw);
	}
}

static void igb_create_lbtest_frame(struct sk_buff *skb,
                                    unsigned int frame_size)
{
	memset(skb->data, 0xFF, frame_size);
	frame_size &= ~1;
	memset(&skb->data[frame_size / 2], 0xAA, frame_size / 2 - 1);
	memset(&skb->data[frame_size / 2 + 10], 0xBE, 1);
	memset(&skb->data[frame_size / 2 + 12], 0xAF, 1);
}

static int igb_check_lbtest_frame(struct sk_buff *skb, unsigned int frame_size)
{
	frame_size &= ~1;
	if (*(skb->data + 3) == 0xFF) {
		if ((*(skb->data + frame_size / 2 + 10) == 0xBE) &&
		   (*(skb->data + frame_size / 2 + 12) == 0xAF)) {
			return 0;
		}
	}
	return 13;
}

static int igb_run_loopback_test(struct igb_adapter *adapter)
{
	struct igb_ring *tx_ring = &adapter->test_tx_ring;
	struct igb_ring *rx_ring = &adapter->test_rx_ring;
	struct pci_dev *pdev = adapter->pdev;
	int i, j, k, l, lc, good_cnt, ret_val = 0;
	unsigned long time;

	E1000_WRITE_REG(&adapter->hw, E1000_RDT, rx_ring->count - 1);

	/* Calculate the loop count based on the largest descriptor ring
	 * The idea is to wrap the largest ring a number of times using 64
	 * send/receive pairs during each loop
	 */

	if (rx_ring->count <= tx_ring->count)
		lc = ((tx_ring->count / 64) * 2) + 1;
	else
		lc = ((rx_ring->count / 64) * 2) + 1;

	k = l = 0;
	for (j = 0; j <= lc; j++) { /* loop count loop */
		for (i = 0; i < 64; i++) { /* send the packets */
			igb_create_lbtest_frame(tx_ring->buffer_info[k].skb,
			                        1024);
			pci_dma_sync_single_for_device(pdev,
				tx_ring->buffer_info[k].dma,
				tx_ring->buffer_info[k].length,
				PCI_DMA_TODEVICE);
			if (unlikely(++k == tx_ring->count)) k = 0;
		}
		E1000_WRITE_REG(&adapter->hw, E1000_TDT, k);
		msleep(200);
		time = jiffies; /* set the start time for the receive */
		good_cnt = 0;
		do { /* receive the sent packets */
			pci_dma_sync_single_for_cpu(pdev,
			                rx_ring->buffer_info[l].dma,
			                IGB_RXBUFFER_2048,
			                PCI_DMA_FROMDEVICE);

			ret_val = igb_check_lbtest_frame(
			                     rx_ring->buffer_info[l].skb, 1024);
			if (!ret_val)
				good_cnt++;
			if (unlikely(++l == rx_ring->count)) l = 0;
			/* time + 20 msecs (200 msecs on 2.4) is more than
			 * enough time to complete the receives, if it's
			 * exceeded, break and error off
			 */
		} while (good_cnt < 64 && jiffies < (time + 20));
		if (good_cnt != 64) {
			ret_val = 13; /* ret_val is the same as mis-compare */
			break;
		}
		if (jiffies >= (time + 20)) {
			ret_val = 14; /* error code for time out error */
			break;
		}
	} /* end loop count loop */
	return ret_val;
}

static int igb_loopback_test(struct igb_adapter *adapter, u64 *data)
{
	/* PHY loopback cannot be performed if SoL/IDER
	 * sessions are active */
	if (e1000_check_reset_block(&adapter->hw)) {
		DPRINTK(DRV, ERR, "Cannot do PHY loopback test "
		        "when SoL/IDER is active.\n");
		*data = 0;
		goto out;
	}
	*data = igb_setup_desc_rings(adapter);
	if (!*data)
		goto out;
	*data = igb_setup_loopback_test(adapter);
	if (!*data)
		goto err_loopback;
	*data = igb_run_loopback_test(adapter);
	igb_loopback_cleanup(adapter);

err_loopback:
	igb_free_desc_rings(adapter);
out:
	return *data;
}

static int igb_link_test(struct igb_adapter *adapter, u64 *data)
{
	*data = 0;
	if (adapter->hw.media_type == e1000_media_type_internal_serdes) {
		int i = 0;
		adapter->hw.mac.serdes_has_link = 0;

		/* On some blade server designs, link establishment
		 * could take as long as 2-3 minutes */
		do {
			e1000_check_for_link(&adapter->hw);
			if (adapter->hw.mac.serdes_has_link == 1)
				return *data;
			msleep(20);
		} while (i++ < 3750);

		*data = 1;
	} else {
		e1000_check_for_link(&adapter->hw);
		if (adapter->hw.mac.autoneg)
			msleep(4000);

		if (!(E1000_READ_REG(&adapter->hw, E1000_STATUS) &
		      E1000_STATUS_LU))
			*data = 1;
	}
	return *data;
}

static int igb_diag_test_count(struct net_device *netdev)
{
	return IGB_TEST_LEN;
}

static void igb_diag_test(struct net_device *netdev,
                          struct ethtool_test *eth_test, u64 *data)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	u16 autoneg_advertised;
	u8 forced_speed_duplex, autoneg;
	bool if_running = netif_running(netdev);

	set_bit(__IGB_TESTING, &adapter->state);
	if (eth_test->flags == ETH_TEST_FL_OFFLINE) {
		/* Offline tests */

		/* save speed, duplex, autoneg settings */
		autoneg_advertised = adapter->hw.phy.autoneg_advertised;
		forced_speed_duplex = adapter->hw.mac.forced_speed_duplex;
		autoneg = adapter->hw.mac.autoneg;

		DPRINTK(HW, INFO, "offline testing starting\n");

		/* Link test performed before hardware reset so autoneg doesn't
		 * interfere with test result */
		if (igb_link_test(adapter, &data[4]))
			eth_test->flags |= ETH_TEST_FL_FAILED;

		if (if_running)
			/* indicate we're in test mode */
			dev_close(netdev);
		else
			igb_reset(adapter);

		if (igb_reg_test(adapter, &data[0]))
			eth_test->flags |= ETH_TEST_FL_FAILED;

		igb_reset(adapter);
		if (igb_eeprom_test(adapter, &data[1]))
			eth_test->flags |= ETH_TEST_FL_FAILED;

		igb_reset(adapter);
		if (igb_intr_test(adapter, &data[2]))
			eth_test->flags |= ETH_TEST_FL_FAILED;

		igb_reset(adapter);
		if (igb_loopback_test(adapter, &data[3]))
			eth_test->flags |= ETH_TEST_FL_FAILED;

		/* restore speed, duplex, autoneg settings */
		adapter->hw.phy.autoneg_advertised = autoneg_advertised;
		adapter->hw.mac.forced_speed_duplex = forced_speed_duplex;
		adapter->hw.mac.autoneg = autoneg;

		/* force this routine to wait until autoneg complete/timeout */
		adapter->hw.phy.wait_for_link = 1;
		igb_reset(adapter);
		adapter->hw.phy.wait_for_link = 0;

		clear_bit(__IGB_TESTING, &adapter->state);
		if (if_running)
			dev_open(netdev);
	} else {
		DPRINTK(HW, INFO, "online testing starting\n");
		/* Online tests */
		if (igb_link_test(adapter, &data[4]))
			eth_test->flags |= ETH_TEST_FL_FAILED;

		/* Online tests aren't run; pass by default */
		data[0] = 0;
		data[1] = 0;
		data[2] = 0;
		data[3] = 0;

		clear_bit(__IGB_TESTING, &adapter->state);
	}
	msleep_interruptible(4 * 1000);
}

static int igb_wol_exclusion(struct igb_adapter *adapter,
                             struct ethtool_wolinfo *wol)
{
	struct e1000_hw *hw = &adapter->hw;
	int retval = 1; /* fail by default */

	switch (hw->device_id) {
	case E1000_DEV_ID_82575EB_COPPER:
	case E1000_DEV_ID_82575EB_FIBER_SERDES:
		/* Wake events not supported on port B */
		if (E1000_READ_REG(hw, E1000_STATUS) & E1000_STATUS_FUNC_1) {
			wol->supported = 0;
			break;
		}
		/* return success for non excluded adapter ports */
		retval = 0;
		break;
	case E1000_DEV_ID_82575GB_QUAD_COPPER:
		/* WoL not supported */
		wol->supported = 0;
		break;
	default:
		/* dual port cards only support WoL on port A from now on
		 * unless it was enabled in the eeprom for port B
		 * so exclude FUNC_1 ports from having WoL enabled */
		if (E1000_READ_REG(hw, E1000_STATUS) & E1000_STATUS_FUNC_1 &&
		    !adapter->eeprom_wol) {
			wol->supported = 0;
			break;
		}

		retval = 0;
	}

	return retval;
}

static void igb_get_wol(struct net_device *netdev, struct ethtool_wolinfo *wol)
{
	struct igb_adapter *adapter = netdev_priv(netdev);

	wol->supported = WAKE_UCAST | WAKE_MCAST |
	                 WAKE_BCAST | WAKE_MAGIC;
	wol->wolopts = 0;

	/* this function will set ->supported = 0 and return 1 if wol is not
	 * supported by this hardware */
	if (igb_wol_exclusion(adapter, wol))
		return;

	/* apply any specific unsupported masks here */
	switch (adapter->hw.device_id) {
	default:
		break;
	}

	if (adapter->wol & E1000_WUFC_EX)
		wol->wolopts |= WAKE_UCAST;
	if (adapter->wol & E1000_WUFC_MC)
		wol->wolopts |= WAKE_MCAST;
	if (adapter->wol & E1000_WUFC_BC)
		wol->wolopts |= WAKE_BCAST;
	if (adapter->wol & E1000_WUFC_MAG)
		wol->wolopts |= WAKE_MAGIC;

	return;
}

static int igb_set_wol(struct net_device *netdev, struct ethtool_wolinfo *wol)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct e1000_hw *hw = &adapter->hw;

	if (wol->wolopts & (WAKE_PHY | WAKE_ARP | WAKE_MAGICSECURE))
		return -EOPNOTSUPP;

	if (igb_wol_exclusion(adapter, wol))
		return wol->wolopts ? -EOPNOTSUPP : 0;

	switch (hw->device_id) {
	default:
		break;
	}

	/* these settings will always override what we currently have */
	adapter->wol = 0;

	if (wol->wolopts & WAKE_UCAST)
		adapter->wol |= E1000_WUFC_EX;
	if (wol->wolopts & WAKE_MCAST)
		adapter->wol |= E1000_WUFC_MC;
	if (wol->wolopts & WAKE_BCAST)
		adapter->wol |= E1000_WUFC_BC;
	if (wol->wolopts & WAKE_MAGIC)
		adapter->wol |= E1000_WUFC_MAG;

	return 0;
}

/* toggle LED 4 times per second = 2 "blinks" per second */
#define IGB_ID_INTERVAL		(HZ/4)

/* bit defines for adapter->led_status */
#define IGB_LED_ON		0

static void igb_led_blink_callback(unsigned long data)
{
	struct igb_adapter *adapter = (struct igb_adapter *) data;

	if (test_and_change_bit(IGB_LED_ON, &adapter->led_status))
		e1000_led_off(&adapter->hw);
	else
		e1000_led_on(&adapter->hw);

	mod_timer(&adapter->blink_timer, jiffies + IGB_ID_INTERVAL);
}

static int igb_phys_id(struct net_device *netdev, u32 data)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct e1000_hw *hw = &adapter->hw;

	if (!data || data > (u32)(MAX_SCHEDULE_TIMEOUT / HZ))
		data = (u32)(MAX_SCHEDULE_TIMEOUT / HZ);

	if (hw->phy.type == e1000_phy_ife) {
		if (!adapter->blink_timer.function) {
			init_timer(&adapter->blink_timer);
			adapter->blink_timer.function = igb_led_blink_callback;
			adapter->blink_timer.data = (unsigned long) adapter;
		}
		mod_timer(&adapter->blink_timer, jiffies);
		msleep_interruptible(data * 1000);
		del_timer_sync(&adapter->blink_timer);
		e1000_write_phy_reg(hw, IFE_PHY_SPECIAL_CONTROL_LED, 0);
	} else {
		e1000_blink_led(hw);
		msleep_interruptible(data * 1000);
	}

	e1000_led_off(hw);
	clear_bit(IGB_LED_ON, &adapter->led_status);
	e1000_cleanup_led(hw);

	return 0;
}

static int igb_nway_reset(struct net_device *netdev)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	if (netif_running(netdev))
		igb_reinit_locked(adapter);
	return 0;
}

static int igb_get_stats_count(struct net_device *netdev)
{
	return IGB_STATS_LEN;
}

static void igb_get_ethtool_stats(struct net_device *netdev,
                                  struct ethtool_stats *stats, u64 *data)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
#if defined(CONFIG_IGB_MQ) || defined (CONFIG_IGB_MQ_RX)
	u64 *queue_stat;
	int stat_count = sizeof(struct igb_queue_stats) / sizeof(u64);
	int j;
#endif
	int i;

	igb_update_stats(adapter);
	for (i = 0; i < IGB_GLOBAL_STATS_LEN; i++) {
		char *p = (char *)adapter+igb_gstrings_stats[i].stat_offset;
		data[i] = (igb_gstrings_stats[i].sizeof_stat ==
			sizeof(u64)) ? *(u64 *)p : *(u32 *)p;
	}
}

static void igb_get_strings(struct net_device *netdev, u32 stringset, u8 *data)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	u8 *p = data;
	int i;

	switch (stringset) {
	case ETH_SS_TEST:
		memcpy(data, *igb_gstrings_test,
			IGB_TEST_LEN*ETH_GSTRING_LEN);
		break;
	case ETH_SS_STATS:
		for (i = 0; i < IGB_GLOBAL_STATS_LEN; i++) {
			memcpy(p, igb_gstrings_stats[i].stat_string,
			       ETH_GSTRING_LEN);
			p += ETH_GSTRING_LEN;
		}
		for (i = 0; i < adapter->num_tx_queues; i++) {
			sprintf(p, "tx_queue_%u_packets", i);
			p += ETH_GSTRING_LEN;
			sprintf(p, "tx_queue_%u_bytes", i);
			p += ETH_GSTRING_LEN;
		}
		for (i = 0; i < adapter->num_rx_queues; i++) {
			sprintf(p, "rx_queue_%u_packets", i);
			p += ETH_GSTRING_LEN;
			sprintf(p, "rx_queue_%u_bytes", i);
			p += ETH_GSTRING_LEN;
		}
/*		BUG_ON(p - data != IGB_STATS_LEN * ETH_GSTRING_LEN); */
		break;
	}
}

static struct ethtool_ops igb_ethtool_ops = {
	.get_settings           = igb_get_settings,
	.set_settings           = igb_set_settings,
	.get_drvinfo            = igb_get_drvinfo,
	.get_regs_len           = igb_get_regs_len,
	.get_regs               = igb_get_regs,
	.get_wol                = igb_get_wol,
	.set_wol                = igb_set_wol,
	.get_msglevel           = igb_get_msglevel,
	.set_msglevel           = igb_set_msglevel,
	.nway_reset             = igb_nway_reset,
	.get_link               = ethtool_op_get_link,
	.get_eeprom_len         = igb_get_eeprom_len,
	.get_eeprom             = igb_get_eeprom,
	.set_eeprom             = igb_set_eeprom,
	.get_ringparam          = igb_get_ringparam,
	.set_ringparam          = igb_set_ringparam,
	.get_pauseparam         = igb_get_pauseparam,
	.set_pauseparam         = igb_set_pauseparam,
	.get_rx_csum            = igb_get_rx_csum,
	.set_rx_csum            = igb_set_rx_csum,
	.get_tx_csum            = igb_get_tx_csum,
	.set_tx_csum            = igb_set_tx_csum,
	.get_sg                 = ethtool_op_get_sg,
	.set_sg                 = ethtool_op_set_sg,
	.get_tso                = ethtool_op_get_tso,
	.set_tso                = igb_set_tso,
	.self_test_count        = igb_diag_test_count,
	.self_test              = igb_diag_test,
	.get_strings            = igb_get_strings,
	.phys_id                = igb_phys_id,
	.get_stats_count        = igb_get_stats_count,
	.get_ethtool_stats      = igb_get_ethtool_stats,
	.get_perm_addr          = ethtool_op_get_perm_addr,
};

void igb_set_ethtool_ops(struct net_device *netdev)
{
	SET_ETHTOOL_OPS(netdev, &igb_ethtool_ops);
}
