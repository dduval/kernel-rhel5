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
#include <asm/div64.h>

MODULE_VERSION(DRV_VER);
MODULE_DEVICE_TABLE(pci, be_dev_ids);
MODULE_DESCRIPTION(DRV_DESC " " DRV_VER);
MODULE_AUTHOR("ServerEngines Corporation");
MODULE_LICENSE("GPL");

static unsigned int rx_frag_size = 2048;
module_param(rx_frag_size, uint, S_IRUGO);
MODULE_PARM_DESC(rx_frag_size, "Size of a fragment that holds rcvd data.");

static unsigned int lro = 1;
module_param(lro, uint, S_IRUGO);
MODULE_PARM_DESC(lro, "Obsolete, only for backward compatibility. Don't use.");

static DEFINE_PCI_DEVICE_TABLE(be_dev_ids) = {
	{ PCI_DEVICE(BE_VENDOR_ID, BE_DEVICE_ID1) },
	{ PCI_DEVICE(BE_VENDOR_ID, BE_DEVICE_ID2) },
	{ PCI_DEVICE(BE_VENDOR_ID, OC_DEVICE_ID1) },
	{ PCI_DEVICE(BE_VENDOR_ID, OC_DEVICE_ID2) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, be_dev_ids);

static void be_queue_free(struct be_adapter *adapter, struct be_queue_info *q)
{
	struct be_dma_mem *mem = &q->dma_mem;
	if (mem->va)
		pci_free_consistent(adapter->pdev, mem->size,
			mem->va, mem->dma);
}

static int be_queue_alloc(struct be_adapter *adapter, struct be_queue_info *q,
		u16 len, u16 entry_size)
{
	struct be_dma_mem *mem = &q->dma_mem;

	memset(q, 0, sizeof(*q));
	q->len = len;
	q->entry_size = entry_size;
	mem->size = len * entry_size;
	mem->va = pci_alloc_consistent(adapter->pdev, mem->size, &mem->dma);
	if (!mem->va)
		return -1;
	memset(mem->va, 0, mem->size);
	return 0;
}

static void be_intr_set(struct be_adapter *adapter, bool enable)
{
	u8 __iomem *addr = adapter->pcicfg + PCICFG_MEMBAR_CTRL_INT_CTRL_OFFSET;
	u32 reg = ioread32(addr);
	u32 enabled = reg & MEMBAR_CTRL_INT_CTRL_HOSTINTR_MASK;

	if (!enabled && enable)
		reg |= MEMBAR_CTRL_INT_CTRL_HOSTINTR_MASK;
	else if (enabled && !enable)
		reg &= ~MEMBAR_CTRL_INT_CTRL_HOSTINTR_MASK;
	else
		return;

	iowrite32(reg, addr);
}

static void be_rxq_notify(struct be_adapter *adapter, u16 qid, u16 posted)
{
	u32 val = 0;
	val |= qid & DB_RQ_RING_ID_MASK;
	val |= posted << DB_RQ_NUM_POSTED_SHIFT;
	iowrite32(val, adapter->db + DB_RQ_OFFSET);
}

static void be_txq_notify(struct be_adapter *adapter, u16 qid, u16 posted)
{
	u32 val = 0;
	val |= qid & DB_TXULP_RING_ID_MASK;
	val |= (posted & DB_TXULP_NUM_POSTED_MASK) << DB_TXULP_NUM_POSTED_SHIFT;
	iowrite32(val, adapter->db + DB_TXULP1_OFFSET);
}

static void be_eq_notify(struct be_adapter *adapter, u16 qid,
		bool arm, bool clear_int, u16 num_popped)
{
	u32 val = 0;
	val |= qid & DB_EQ_RING_ID_MASK;
	if (arm)
		val |= 1 << DB_EQ_REARM_SHIFT;
	if (clear_int)
		val |= 1 << DB_EQ_CLR_SHIFT;
	val |= 1 << DB_EQ_EVNT_SHIFT;
	val |= num_popped << DB_EQ_NUM_POPPED_SHIFT;
	iowrite32(val, adapter->db + DB_EQ_OFFSET);
}

void be_cq_notify(struct be_adapter *adapter, u16 qid, bool arm, u16 num_popped)
{
	u32 val = 0;
	val |= qid & DB_CQ_RING_ID_MASK;
	if (arm)
		val |= 1 << DB_CQ_REARM_SHIFT;
	val |= num_popped << DB_CQ_NUM_POPPED_SHIFT;
	iowrite32(val, adapter->db + DB_CQ_OFFSET);
}

static int be_mac_addr_set(struct net_device *netdev, void *p)
{
	struct be_adapter *adapter = netdev_priv(netdev);
	struct sockaddr *addr = p;
	int status = 0;

	status = be_cmd_pmac_del(adapter, adapter->if_handle, adapter->pmac_id);
	if (status)
		return status;

	status = be_cmd_pmac_add(adapter, (u8 *)addr->sa_data,
			adapter->if_handle, &adapter->pmac_id);
	if (!status)
		memcpy(netdev->dev_addr, addr->sa_data, netdev->addr_len);

	return status;
}

void netdev_stats_update(struct be_adapter *adapter)
{
	struct be_hw_stats *hw_stats = hw_stats_from_cmd(adapter->stats.cmd.va);
	struct be_rxf_stats *rxf_stats = &hw_stats->rxf;
	struct be_port_rxf_stats *port_stats =
			&rxf_stats->port[adapter->port_num];
	struct net_device_stats *dev_stats = &adapter->stats.net_stats;
	struct be_erx_stats *erx_stats = &hw_stats->erx;

	dev_stats->rx_packets = port_stats->rx_total_frames;
	dev_stats->tx_packets = port_stats->tx_unicastframes +
		port_stats->tx_multicastframes + port_stats->tx_broadcastframes;
	dev_stats->rx_bytes = (u64) port_stats->rx_bytes_msd << 32 |
				(u64) port_stats->rx_bytes_lsd;
	dev_stats->tx_bytes = (u64) port_stats->tx_bytes_msd << 32 |
				(u64) port_stats->tx_bytes_lsd;

	/* bad pkts received */
	dev_stats->rx_errors = port_stats->rx_crc_errors +
		port_stats->rx_alignment_symbol_errors +
		port_stats->rx_in_range_errors +
		port_stats->rx_out_range_errors +
		port_stats->rx_frame_too_long +
		port_stats->rx_dropped_too_small +
		port_stats->rx_dropped_too_short +
		port_stats->rx_dropped_header_too_small +
		port_stats->rx_dropped_tcp_length +
		port_stats->rx_dropped_runt +
		port_stats->rx_tcp_checksum_errs +
		port_stats->rx_ip_checksum_errs +
		port_stats->rx_udp_checksum_errs;

	/*  no space in linux buffers: best possible approximation */
	dev_stats->rx_dropped =
		erx_stats->rx_drops_no_fragments[adapter->rx_obj.q.id];

	/* detailed rx errors */
	dev_stats->rx_length_errors = port_stats->rx_in_range_errors +
		port_stats->rx_out_range_errors +
		port_stats->rx_frame_too_long;

	/* receive ring buffer overflow */
	dev_stats->rx_over_errors = 0;

	dev_stats->rx_crc_errors = port_stats->rx_crc_errors;

	/* frame alignment errors */
	dev_stats->rx_frame_errors = port_stats->rx_alignment_symbol_errors;

	/* receiver fifo overrun */
	/* drops_no_pbuf is no per i/f, it's per BE card */
	dev_stats->rx_fifo_errors = port_stats->rx_fifo_overflow +
					port_stats->rx_input_fifo_overflow +
					rxf_stats->rx_drops_no_pbuf;
	/* receiver missed packetd */
	dev_stats->rx_missed_errors = 0;

	/*  packet transmit problems */
	dev_stats->tx_errors = 0;

	/* no space available in linux */
	dev_stats->tx_dropped = 0;

	dev_stats->multicast = port_stats->rx_multicast_frames;
	dev_stats->collisions = 0;

	/* detailed tx_errors */
	dev_stats->tx_aborted_errors = 0;
	dev_stats->tx_carrier_errors = 0;
	dev_stats->tx_fifo_errors = 0;
	dev_stats->tx_heartbeat_errors = 0;
	dev_stats->tx_window_errors = 0;
}

void be_link_status_update(struct be_adapter *adapter, bool link_up)
{
	struct net_device *netdev = adapter->netdev;

	/* If link came up or went down */
	if (adapter->link_up != link_up) {
		adapter->link_speed = -1;
		if (link_up) {
			netif_start_queue(netdev);
			netif_carrier_on(netdev);
			printk(KERN_INFO "%s: Link up\n", netdev->name);
		} else {
			netif_stop_queue(netdev);
			netif_carrier_off(netdev);
			printk(KERN_INFO "%s: Link down\n", netdev->name);
		}
		adapter->link_up = link_up;
	}
}

/* Update the EQ delay n BE based on the RX frags consumed / sec */
static void be_eqd_update(struct be_adapter *adapter)
{
	struct be_eq_obj *be_eq = &adapter->be_eq;
	struct be_drvr_stats *stats = &adapter->stats.drvr_stats;
	ulong now = jiffies;
	u32 eqd = 0;

	if (!be_eq->enable_aic)
		return;

	/* Wrapped around */
	if (time_before(now, stats->be_eps_jiffies)) {
		stats->be_eps_jiffies = now;
		return;
	}

	/* Update once a second */
	if ((now - stats->be_eps_jiffies) < HZ)
		return;

	stats->be_eps = (stats->be_num_events - stats->be_num_events_prev) /
			((now - stats->be_eps_jiffies) / HZ);

	stats->be_eps_jiffies = now;
	stats->be_num_events_prev = stats->be_num_events;

	eqd = be_eq->cur_eqd;

	if ((stats->be_eps > 20000) && (eqd < be_eq->max_eqd))
		eqd += 8;
	if ((stats->be_eps < 10000) && (eqd > be_eq->min_eqd))
		eqd -= 8;

	if (eqd != be_eq->cur_eqd) {
		be_cmd_modify_eqd(adapter, be_eq->q.id, eqd);
		be_eq->cur_eqd = eqd;
	}
}

static struct net_device_stats *be_get_stats(struct net_device *dev)
{
	struct be_adapter *adapter = netdev_priv(dev);

	return &adapter->stats.net_stats;
}

static u32 be_calc_rate(u64 bytes, unsigned long ticks)
{
	u64 rate = bytes;

	do_div(rate, ticks / HZ);
	rate <<= 3;			/* bytes/sec -> bits/sec */
	do_div(rate, 1000000ul);	/* MB/Sec */

	return rate;
}

static void be_tx_rate_update(struct be_adapter *adapter)
{
	struct be_drvr_stats *stats = drvr_stats(adapter);
	ulong now = jiffies;

	/* Wrapped around? */
	if (time_before(now, stats->be_tx_jiffies)) {
		stats->be_tx_jiffies = now;
		return;
	}

	/* Update tx rate once in two seconds */
	if ((now - stats->be_tx_jiffies) > 2 * HZ) {
		stats->be_tx_rate = be_calc_rate(stats->be_tx_bytes
						  - stats->be_tx_bytes_prev,
						 now - stats->be_tx_jiffies);
		stats->be_tx_jiffies = now;
		stats->be_tx_bytes_prev = stats->be_tx_bytes;
	}
}

static void be_tx_stats_update(struct be_adapter *adapter,
			u32 wrb_cnt, u32 copied, bool stopped)
{
	struct be_drvr_stats *stats = drvr_stats(adapter);
	stats->be_tx_reqs++;
	stats->be_tx_wrbs += wrb_cnt;
	stats->be_tx_bytes += copied;
	if (stopped)
		stats->be_tx_stops++;
}

/* Determine number of WRB entries needed to xmit data in an skb */
static u32 wrb_cnt_for_skb(struct sk_buff *skb, bool *dummy)
{
	int cnt = (skb->len > skb->data_len);

	cnt += skb_shinfo(skb)->nr_frags;

	/* to account for hdr wrb */
	cnt++;
	if (cnt & 1) {
		/* add a dummy to make it an even num */
		cnt++;
		*dummy = true;
	} else
		*dummy = false;
	BUG_ON(cnt > BE_MAX_TX_FRAG_COUNT);
	return cnt;
}

static inline void wrb_fill(struct be_eth_wrb *wrb, u64 addr, int len)
{
	wrb->frag_pa_hi = upper_32_bits(addr);
	wrb->frag_pa_lo = addr & 0xFFFFFFFF;
	wrb->frag_len = len & ETH_WRB_FRAG_LEN_MASK;
}

static void wrb_fill_hdr(struct be_eth_hdr_wrb *hdr, struct sk_buff *skb,
		bool vlan, u32 wrb_cnt, u32 len)
{
	memset(hdr, 0, sizeof(*hdr));

	AMAP_SET_BITS(struct amap_eth_hdr_wrb, crc, hdr, 1);

	if (skb_shinfo(skb)->gso_segs > 1 && skb_shinfo(skb)->gso_size) {
		AMAP_SET_BITS(struct amap_eth_hdr_wrb, lso, hdr, 1);
		AMAP_SET_BITS(struct amap_eth_hdr_wrb, lso_mss,
			hdr, skb_shinfo(skb)->gso_size);
	} else if (skb->ip_summed == CHECKSUM_PARTIAL) {
		if (is_tcp_pkt(skb))
			AMAP_SET_BITS(struct amap_eth_hdr_wrb, tcpcs, hdr, 1);
		else if (is_udp_pkt(skb))
			AMAP_SET_BITS(struct amap_eth_hdr_wrb, udpcs, hdr, 1);
	}

	if (vlan && vlan_tx_tag_present(skb)) {
		AMAP_SET_BITS(struct amap_eth_hdr_wrb, vlan, hdr, 1);
		AMAP_SET_BITS(struct amap_eth_hdr_wrb, vlan_tag,
			hdr, vlan_tx_tag_get(skb));
	}

	AMAP_SET_BITS(struct amap_eth_hdr_wrb, event, hdr, 1);
	AMAP_SET_BITS(struct amap_eth_hdr_wrb, complete, hdr, 1);
	AMAP_SET_BITS(struct amap_eth_hdr_wrb, num_wrb, hdr, wrb_cnt);
	AMAP_SET_BITS(struct amap_eth_hdr_wrb, len, hdr, len);
}


static int make_tx_wrbs(struct be_adapter *adapter,
		struct sk_buff *skb, u32 wrb_cnt, bool dummy_wrb)
{
	u64 busaddr;
	u32 i, copied = 0;
	struct pci_dev *pdev = adapter->pdev;
	struct sk_buff *first_skb = skb;
	struct be_queue_info *txq = &adapter->tx_obj.q;
	struct be_eth_wrb *wrb;
	struct be_eth_hdr_wrb *hdr;

	atomic_add(wrb_cnt, &txq->used);
	hdr = queue_head_node(txq);
	queue_head_inc(txq);

	if (skb->len > skb->data_len) {
		int len = skb->len - skb->data_len;
		busaddr = pci_map_single(pdev, skb->data, len,
					 PCI_DMA_TODEVICE);
		wrb = queue_head_node(txq);
		wrb_fill(wrb, busaddr, len);
		be_dws_cpu_to_le(wrb, sizeof(*wrb));
		queue_head_inc(txq);
		copied += len;
	}

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		struct skb_frag_struct *frag =
			&skb_shinfo(skb)->frags[i];
		busaddr = pci_map_page(pdev, frag->page,
				       frag->page_offset,
				       frag->size, PCI_DMA_TODEVICE);
		wrb = queue_head_node(txq);
		wrb_fill(wrb, busaddr, frag->size);
		be_dws_cpu_to_le(wrb, sizeof(*wrb));
		queue_head_inc(txq);
		copied += frag->size;
	}

	if (dummy_wrb) {
		wrb = queue_head_node(txq);
		wrb_fill(wrb, 0, 0);
		be_dws_cpu_to_le(wrb, sizeof(*wrb));
		queue_head_inc(txq);
	}

	wrb_fill_hdr(hdr, first_skb, adapter->vlan_grp ? true : false,
		wrb_cnt, copied);
	be_dws_cpu_to_le(hdr, sizeof(*hdr));

	return copied;
}

static int be_xmit(struct sk_buff *skb,
				 struct net_device *netdev)

{
	struct be_adapter *adapter = netdev_priv(netdev);
	struct be_tx_obj *tx_obj = &adapter->tx_obj;
	struct be_queue_info *txq = &tx_obj->q;
	u32 wrb_cnt = 0, copied = 0;
	u32 start = txq->head;
	bool dummy_wrb, stopped = false;

	wrb_cnt = wrb_cnt_for_skb(skb, &dummy_wrb);

	copied = make_tx_wrbs(adapter, skb, wrb_cnt, dummy_wrb);
	if (copied) {
		/* record the sent skb in the sent_skb table */
		BUG_ON(tx_obj->sent_skb_list[start]);
		tx_obj->sent_skb_list[start] = skb;

		/* Ensure txq has space for the next skb; Else stop the queue
		 * *BEFORE* ringing the tx doorbell, so that we serialze the
		 * tx compls of the current transmit which'll wake up the queue
		 */
		if ((BE_MAX_TX_FRAG_COUNT + atomic_read(&txq->used)) >=
								txq->len) {
			netif_stop_queue(netdev);
			stopped = true;
		}

		be_txq_notify(adapter, txq->id, wrb_cnt);

		be_tx_stats_update(adapter, wrb_cnt, copied, stopped);
	} else {
		txq->head = start;
		dev_kfree_skb_any(skb);
	}
	return NETDEV_TX_OK;
}

static int be_change_mtu(struct net_device *netdev, int new_mtu)
{
	struct be_adapter *adapter = netdev_priv(netdev);
	if (new_mtu < BE_MIN_MTU ||
			new_mtu > (BE_MAX_JUMBO_FRAME_SIZE -
					(ETH_HLEN + ETH_FCS_LEN))) {
		dev_info(&adapter->pdev->dev,
			"MTU must be between %d and %d bytes\n",
			BE_MIN_MTU,
			(BE_MAX_JUMBO_FRAME_SIZE - (ETH_HLEN + ETH_FCS_LEN)));
		return -EINVAL;
	}
	dev_info(&adapter->pdev->dev, "MTU changed from %d to %d bytes\n",
			netdev->mtu, new_mtu);
	netdev->mtu = new_mtu;
	return 0;
}

/*
 * if there are BE_NUM_VLANS_SUPPORTED or lesser number of VLANS configured,
 * program them in BE.  If more than BE_NUM_VLANS_SUPPORTED are configured,
 * set the BE in promiscuous VLAN mode.
 */
static int be_vid_config(struct be_adapter *adapter)
{
	u16 vtag[BE_NUM_VLANS_SUPPORTED];
	u16 ntags = 0, i;
	int status;

	if (adapter->num_vlans <= BE_NUM_VLANS_SUPPORTED)  {
		/* Construct VLAN Table to give to HW */
		for (i = 0; i < VLAN_GROUP_ARRAY_LEN; i++) {
			if (adapter->vlan_tag[i]) {
				vtag[ntags] = cpu_to_le16(i);
				ntags++;
			}
		}
		status = be_cmd_vlan_config(adapter, adapter->if_handle,
					vtag, ntags, 1, 0);
	} else {
		status = be_cmd_vlan_config(adapter, adapter->if_handle,
					NULL, 0, 1, 1);
	}
	return status;
}

static void be_vlan_register(struct net_device *netdev, struct vlan_group *grp)
{
	struct be_adapter *adapter = netdev_priv(netdev);
	struct be_eq_obj *be_eq = &adapter->be_eq;

	be_eq_notify(adapter, be_eq->q.id, false, false, 0);
	adapter->vlan_grp = grp;
	be_eq_notify(adapter, be_eq->q.id, true, false, 0);
}

static void be_vlan_add_vid(struct net_device *netdev, u16 vid)
{
	struct be_adapter *adapter = netdev_priv(netdev);

	adapter->num_vlans++;
	adapter->vlan_tag[vid] = 1;

	be_vid_config(adapter);
}

static void be_vlan_rem_vid(struct net_device *netdev, u16 vid)
{
	struct be_adapter *adapter = netdev_priv(netdev);

	adapter->num_vlans--;
	adapter->vlan_tag[vid] = 0;

	vlan_group_set_device(adapter->vlan_grp, vid, NULL);
	be_vid_config(adapter);
}

static void be_set_multicast_list(struct net_device *netdev)
{
	struct be_adapter *adapter = netdev_priv(netdev);

	if (netdev->flags & IFF_PROMISC) {
		be_cmd_promiscuous_config(adapter, adapter->port_num, 1);
		adapter->promiscuous = true;
		goto done;
	}

	/* BE was previously in promiscous mode; disable it */
	if (adapter->promiscuous) {
		adapter->promiscuous = false;
		be_cmd_promiscuous_config(adapter, adapter->port_num, 0);
	}

	/* Enable multicast promisc if num configured exceeds what we support */
	if (netdev->flags & IFF_ALLMULTI || netdev->mc_count > BE_MAX_MC) {
		be_cmd_multicast_set(adapter, adapter->if_handle, NULL, 0,
				&adapter->mc_cmd_mem);
		goto done;
	}

	be_cmd_multicast_set(adapter, adapter->if_handle, netdev->mc_list,
		netdev->mc_count, &adapter->mc_cmd_mem);
done:
	return;
}

static void be_rx_rate_update(struct be_adapter *adapter)
{
	struct be_drvr_stats *stats = drvr_stats(adapter);
	ulong now = jiffies;

	/* Wrapped around */
	if (time_before(now, stats->be_rx_jiffies)) {
		stats->be_rx_jiffies = now;
		return;
	}

	/* Update the rate once in two seconds */
	if ((now - stats->be_rx_jiffies) < 2 * HZ)
		return;

	stats->be_rx_rate = be_calc_rate(stats->be_rx_bytes
					  - stats->be_rx_bytes_prev,
					 now - stats->be_rx_jiffies);
	stats->be_rx_jiffies = now;
	stats->be_rx_bytes_prev = stats->be_rx_bytes;
}

static void be_rx_stats_update(struct be_adapter *adapter,
		u32 pktsize, u16 numfrags)
{
	struct be_drvr_stats *stats = drvr_stats(adapter);

	stats->be_rx_compl++;
	stats->be_rx_frags += numfrags;
	stats->be_rx_bytes += pktsize;
	stats->be_rx_pkts++;
}

static inline bool do_pkt_csum(struct be_eth_rx_compl *rxcp, bool cso)
{
	u8 l4_cksm, ip_version, ipcksm, tcpf = 0, udpf = 0, ipv6_chk;

	l4_cksm = AMAP_GET_BITS(struct amap_eth_rx_compl, l4_cksm, rxcp);
	ipcksm = AMAP_GET_BITS(struct amap_eth_rx_compl, ipcksm, rxcp);
	ip_version = AMAP_GET_BITS(struct amap_eth_rx_compl, ip_version, rxcp);
	if (ip_version) {
		tcpf = AMAP_GET_BITS(struct amap_eth_rx_compl, tcpf, rxcp);
		udpf = AMAP_GET_BITS(struct amap_eth_rx_compl, udpf, rxcp);
	}
	ipv6_chk = (ip_version && (tcpf || udpf));

	return ((l4_cksm && ipv6_chk && ipcksm) && cso) ? false : true;
}

static struct be_rx_page_info *
get_rx_page_info(struct be_adapter *adapter, u16 frag_idx)
{
	struct be_rx_page_info *rx_page_info;
	struct be_queue_info *rxq = &adapter->rx_obj.q;

	rx_page_info = &adapter->rx_obj.page_info_tbl[frag_idx];
	BUG_ON(!rx_page_info->page);

	if (rx_page_info->last_page_user) {
		pci_unmap_page(adapter->pdev, pci_unmap_addr(rx_page_info, bus),
			adapter->big_page_size, PCI_DMA_FROMDEVICE);
		rx_page_info->last_page_user = false;
	}

	atomic_dec(&rxq->used);
	return rx_page_info;
}

/* Throwaway the data in the Rx completion */
static void be_rx_compl_discard(struct be_adapter *adapter,
			struct be_eth_rx_compl *rxcp)
{
	struct be_queue_info *rxq = &adapter->rx_obj.q;
	struct be_rx_page_info *page_info;
	u16 rxq_idx, i, num_rcvd;

	rxq_idx = AMAP_GET_BITS(struct amap_eth_rx_compl, fragndx, rxcp);
	num_rcvd = AMAP_GET_BITS(struct amap_eth_rx_compl, numfrags, rxcp);

	for (i = 0; i < num_rcvd; i++) {
		page_info = get_rx_page_info(adapter, rxq_idx);
		put_page(page_info->page);
		memset(page_info, 0, sizeof(*page_info));
		index_inc(&rxq_idx, rxq->len);
	}
}

/*
 * skb_fill_rx_data forms a complete skb for an ether frame
 * indicated by rxcp.
 */
static void skb_fill_rx_data(struct be_adapter *adapter,
			struct sk_buff *skb, struct be_eth_rx_compl *rxcp)
{
	struct be_queue_info *rxq = &adapter->rx_obj.q;
	struct be_rx_page_info *page_info;
	u16 rxq_idx, i, num_rcvd, j;
	u32 pktsize, hdr_len, curr_frag_len, size;
	u8 *start;

	rxq_idx = AMAP_GET_BITS(struct amap_eth_rx_compl, fragndx, rxcp);
	pktsize = AMAP_GET_BITS(struct amap_eth_rx_compl, pktsize, rxcp);
	num_rcvd = AMAP_GET_BITS(struct amap_eth_rx_compl, numfrags, rxcp);

	page_info = get_rx_page_info(adapter, rxq_idx);

	start = page_address(page_info->page) + page_info->page_offset;
	prefetch(start);

	/* Copy data in the first descriptor of this completion */
	curr_frag_len = min(pktsize, rx_frag_size);

	/* Copy the header portion into skb_data */
	hdr_len = min((u32)BE_HDR_LEN, curr_frag_len);
	memcpy(skb->data, start, hdr_len);
	skb->len = curr_frag_len;
	if (curr_frag_len <= BE_HDR_LEN) { /* tiny packet */
		/* Complete packet has now been moved to data */
		put_page(page_info->page);
		skb->data_len = 0;
		skb->tail += curr_frag_len;
	} else {
		skb_shinfo(skb)->nr_frags = 1;
		skb_shinfo(skb)->frags[0].page = page_info->page;
		skb_shinfo(skb)->frags[0].page_offset =
					page_info->page_offset + hdr_len;
		skb_shinfo(skb)->frags[0].size = curr_frag_len - hdr_len;
		skb->data_len = curr_frag_len - hdr_len;
		skb->tail += hdr_len;
	}
	page_info->page = NULL;

	if (pktsize <= rx_frag_size) {
		BUG_ON(num_rcvd != 1);
		goto done;
	}

	/* More frags present for this completion */
	size = pktsize;
	for (i = 1, j = 0; i < num_rcvd; i++) {
		size -= curr_frag_len;
		index_inc(&rxq_idx, rxq->len);
		page_info = get_rx_page_info(adapter, rxq_idx);

		curr_frag_len = min(size, rx_frag_size);

		/* Coalesce all frags from the same physical page in one slot */
		if (page_info->page_offset == 0) {
			/* Fresh page */
			j++;
			skb_shinfo(skb)->frags[j].page = page_info->page;
			skb_shinfo(skb)->frags[j].page_offset =
							page_info->page_offset;
			skb_shinfo(skb)->frags[j].size = 0;
			skb_shinfo(skb)->nr_frags++;
		} else {
			put_page(page_info->page);
		}

		skb_shinfo(skb)->frags[j].size += curr_frag_len;
		skb->len += curr_frag_len;
		skb->data_len += curr_frag_len;

		page_info->page = NULL;
	}
	BUG_ON(j > MAX_SKB_FRAGS);

done:
	be_rx_stats_update(adapter, pktsize, num_rcvd);
	return;
}

/* Process the RX completion indicated by rxcp when GRO is disabled */
static void be_rx_compl_process(struct be_adapter *adapter,
			struct be_eth_rx_compl *rxcp)
{
	struct sk_buff *skb;
	u32 vlanf, vid;
	u8 vtm;

	vlanf = AMAP_GET_BITS(struct amap_eth_rx_compl, vtp, rxcp);
	vtm = AMAP_GET_BITS(struct amap_eth_rx_compl, vtm, rxcp);

	/* vlanf could be wrongly set in some cards.
	 * ignore if vtm is not set */
	if ((adapter->cap & 0x400) && !vtm)
		vlanf = 0;

	skb = netdev_alloc_skb(adapter->netdev, BE_HDR_LEN + NET_IP_ALIGN);
	if (!skb) {
		if (net_ratelimit())
			dev_warn(&adapter->pdev->dev, "skb alloc failed\n");
		be_rx_compl_discard(adapter, rxcp);
		return;
	}

	skb_reserve(skb, NET_IP_ALIGN);

	skb_fill_rx_data(adapter, skb, rxcp);

	if (do_pkt_csum(rxcp, adapter->rx_csum))
		skb->ip_summed = CHECKSUM_NONE;
	else
		skb->ip_summed = CHECKSUM_UNNECESSARY;

	skb->truesize = skb->len + sizeof(struct sk_buff);
	skb->protocol = eth_type_trans(skb, adapter->netdev);
	skb->dev = adapter->netdev;

	if (vlanf) {
		if (!adapter->vlan_grp || adapter->num_vlans == 0) {
			kfree_skb(skb);
			return;
		}
		vid = AMAP_GET_BITS(struct amap_eth_rx_compl, vlan_tag, rxcp);
		vid = be16_to_cpu(vid);
		vlan_hwaccel_receive_skb(skb, adapter->vlan_grp, vid);
	} else {
		netif_receive_skb(skb);
	}

	adapter->netdev->last_rx = jiffies;
	return;
}

/* Process the RX completion indicated by rxcp when GRO is enabled */
static void be_rx_compl_process_gro(struct be_adapter *adapter,
			struct be_eth_rx_compl *rxcp)
{
	struct be_rx_page_info *page_info;
	struct sk_buff *skb = NULL;
	struct be_queue_info *rxq = &adapter->rx_obj.q;
	u32 num_rcvd, pkt_size, remaining, vlanf, curr_frag_len;
	u16 i, rxq_idx = 0, vid, j;
	u8 vtm;

	num_rcvd = AMAP_GET_BITS(struct amap_eth_rx_compl, numfrags, rxcp);
	pkt_size = AMAP_GET_BITS(struct amap_eth_rx_compl, pktsize, rxcp);
	vlanf = AMAP_GET_BITS(struct amap_eth_rx_compl, vtp, rxcp);
	rxq_idx = AMAP_GET_BITS(struct amap_eth_rx_compl, fragndx, rxcp);
	vtm = AMAP_GET_BITS(struct amap_eth_rx_compl, vtm, rxcp);

	/* vlanf could be wrongly set in some cards.
	 * ignore if vtm is not set */
	if ((adapter->cap & 0x400) && !vtm)
		vlanf = 0;

	skb = napi_get_frags(&adapter->napi);
	if (!skb) {
		be_rx_compl_discard(adapter, rxcp);
		return;
	}

	remaining = pkt_size;
	for (i = 0, j = -1; i < num_rcvd; i++) {
		page_info = get_rx_page_info(adapter, rxq_idx);

		curr_frag_len = min(remaining, rx_frag_size);

		/* Coalesce all frags from the same physical page in one slot */
		if (i == 0 || page_info->page_offset == 0) {
			/* First frag or Fresh page */
			j++;
			skb_shinfo(skb)->frags[j].page = page_info->page;
			skb_shinfo(skb)->frags[j].page_offset =
							page_info->page_offset;
			skb_shinfo(skb)->frags[j].size = 0;
		} else {
			put_page(page_info->page);
		}
		skb_shinfo(skb)->frags[j].size += curr_frag_len;

		remaining -= curr_frag_len;
		index_inc(&rxq_idx, rxq->len);
		memset(page_info, 0, sizeof(*page_info));
	}
	BUG_ON(j > MAX_SKB_FRAGS);

	skb_shinfo(skb)->nr_frags = j + 1;
	skb->len = pkt_size;
	skb->data_len = pkt_size;
	skb->truesize += pkt_size;
	skb->ip_summed = CHECKSUM_UNNECESSARY;

	if (likely(!vlanf)) {
		napi_gro_frags(&adapter->napi);
	} else {
		vid = AMAP_GET_BITS(struct amap_eth_rx_compl, vlan_tag, rxcp);
		vid = be16_to_cpu(vid);

		if (!adapter->vlan_grp || adapter->num_vlans == 0)
			return;

		vlan_gro_frags(&adapter->napi, adapter->vlan_grp, vid);
	}

	be_rx_stats_update(adapter, pkt_size, num_rcvd);
	return;
}

static struct be_eth_rx_compl *be_rx_compl_get(struct be_adapter *adapter)
{
	struct be_eth_rx_compl *rxcp = queue_tail_node(&adapter->rx_obj.cq);

	if (rxcp->dw[offsetof(struct amap_eth_rx_compl, valid) / 32] == 0)
		return NULL;

	be_dws_le_to_cpu(rxcp, sizeof(*rxcp));

	queue_tail_inc(&adapter->rx_obj.cq);
	return rxcp;
}

/* To reset the valid bit, we need to reset the whole word as
 * when walking the queue the valid entries are little-endian
 * and invalid entries are host endian
 */
static inline void be_rx_compl_reset(struct be_eth_rx_compl *rxcp)
{
	rxcp->dw[offsetof(struct amap_eth_rx_compl, valid) / 32] = 0;
}

static inline struct page *be_alloc_pages(u32 size)
{
	gfp_t alloc_flags = GFP_ATOMIC;
	u32 order = get_order(size);
	if (order > 0)
		alloc_flags |= __GFP_COMP;
	return  alloc_pages(alloc_flags, order);
}

/*
 * Allocate a page, split it to fragments of size rx_frag_size and post as
 * receive buffers to BE
 */
static void be_post_rx_frags(struct be_adapter *adapter)
{
	struct be_rx_page_info *page_info_tbl = adapter->rx_obj.page_info_tbl;
	struct be_rx_page_info *page_info = NULL, *prev_page_info = NULL;
	struct be_queue_info *rxq = &adapter->rx_obj.q;
	struct page *pagep = NULL;
	struct be_eth_rx_d *rxd;
	u64 page_dmaaddr = 0, frag_dmaaddr;
	u32 posted, page_offset = 0;

	page_info = &page_info_tbl[rxq->head];
	for (posted = 0; posted < MAX_RX_POST && !page_info->page; posted++) {
		if (!pagep) {
			pagep = be_alloc_pages(adapter->big_page_size);
			if (unlikely(!pagep)) {
				drvr_stats(adapter)->be_ethrx_post_fail++;
				break;
			}
			page_dmaaddr = pci_map_page(adapter->pdev, pagep, 0,
						adapter->big_page_size,
						PCI_DMA_FROMDEVICE);
			page_info->page_offset = 0;
		} else {
			get_page(pagep);
			page_info->page_offset = page_offset + rx_frag_size;
		}
		page_offset = page_info->page_offset;
		page_info->page = pagep;
		pci_unmap_addr_set(page_info, bus, page_dmaaddr);
		frag_dmaaddr = page_dmaaddr + page_info->page_offset;

		rxd = queue_head_node(rxq);
		rxd->fragpa_lo = cpu_to_le32(frag_dmaaddr & 0xFFFFFFFF);
		rxd->fragpa_hi = cpu_to_le32(upper_32_bits(frag_dmaaddr));

		/* Any space left in the current big page for another frag? */
		if ((page_offset + rx_frag_size + rx_frag_size) >
					adapter->big_page_size) {
			pagep = NULL;
			page_info->last_page_user = true;
		}

		prev_page_info = page_info;
		queue_head_inc(rxq);
		page_info = &page_info_tbl[rxq->head];
	}
	if (pagep)
		prev_page_info->last_page_user = true;

	if (posted) {
		atomic_add(posted, &rxq->used);
		be_rxq_notify(adapter, rxq->id, posted);
	} else if (atomic_read(&rxq->used) == 0) {
		/* Let be_worker replenish when memory is available */
		adapter->rx_post_starved = true;
	}

	return;
}

static struct be_eth_tx_compl *be_tx_compl_get(struct be_queue_info *tx_cq)
{
	struct be_eth_tx_compl *txcp = queue_tail_node(tx_cq);

	if (txcp->dw[offsetof(struct amap_eth_tx_compl, valid) / 32] == 0)
		return NULL;

	be_dws_le_to_cpu(txcp, sizeof(*txcp));

	txcp->dw[offsetof(struct amap_eth_tx_compl, valid) / 32] = 0;

	queue_tail_inc(tx_cq);
	return txcp;
}

static void be_tx_compl_process(struct be_adapter *adapter, u16 last_index)
{
	struct be_queue_info *txq = &adapter->tx_obj.q;
	struct be_eth_wrb *wrb;
	struct sk_buff **sent_skbs = adapter->tx_obj.sent_skb_list;
	struct sk_buff *sent_skb;
	u64 busaddr;
	u16 cur_index, num_wrbs = 0;

	cur_index = txq->tail;
	sent_skb = sent_skbs[cur_index];
	BUG_ON(!sent_skb);
	sent_skbs[cur_index] = NULL;

	do {
		cur_index = txq->tail;
		wrb = queue_tail_node(txq);
		be_dws_le_to_cpu(wrb, sizeof(*wrb));
		busaddr = ((u64)wrb->frag_pa_hi << 32) | (u64)wrb->frag_pa_lo;
		if (busaddr != 0) {
			pci_unmap_single(adapter->pdev, busaddr,
				wrb->frag_len, PCI_DMA_TODEVICE);
		}
		num_wrbs++;
		queue_tail_inc(txq);
	} while (cur_index != last_index);

	atomic_sub(num_wrbs, &txq->used);

	kfree_skb(sent_skb);
}

static inline struct be_eq_entry *event_get(struct be_eq_obj *eq_obj)
{
	struct be_eq_entry *eqe = queue_tail_node(&eq_obj->q);

	if (!eqe->evt)
		return NULL;

	eqe->evt = le32_to_cpu(eqe->evt);
	queue_tail_inc(&eq_obj->q);
	eqe->evt = 0;
	return eqe;
}

/* Just read and notify events without processing them.
 * Used at the time of destroying event queues */
static void be_eq_clean(struct be_adapter *adapter,
			struct be_eq_obj *eq_obj)
{
	struct be_eq_entry *eqe;
	u16 num = 0;

	while ((eqe = event_get(eq_obj)) != NULL)
		num++;

	if (num)
		be_eq_notify(adapter, eq_obj->q.id, false, true, num);
}

static void be_rx_q_clean(struct be_adapter *adapter)
{
	struct be_rx_page_info *page_info;
	struct be_queue_info *rxq = &adapter->rx_obj.q;
	struct be_queue_info *rx_cq = &adapter->rx_obj.cq;
	struct be_eth_rx_compl *rxcp;
	u16 tail;

	/* First cleanup pending rx completions */
	while ((rxcp = be_rx_compl_get(adapter)) != NULL) {
		be_rx_compl_discard(adapter, rxcp);
		be_rx_compl_reset(rxcp);
		be_cq_notify(adapter, rx_cq->id, true, 1);
	}

	/* Then free posted rx buffer that were not used */
	tail = (rxq->head + rxq->len - atomic_read(&rxq->used)) % rxq->len;
	for (; atomic_read(&rxq->used) > 0; index_inc(&tail, rxq->len)) {
		page_info = get_rx_page_info(adapter, tail);
		put_page(page_info->page);
		memset(page_info, 0, sizeof(*page_info));
	}
	BUG_ON(atomic_read(&rxq->used));
}

static void be_tx_compl_clean(struct be_adapter *adapter)
{
	struct be_queue_info *tx_cq = &adapter->tx_obj.cq;
	struct be_queue_info *txq = &adapter->tx_obj.q;
	struct be_eth_tx_compl *txcp;
	u16 end_idx, cmpl = 0, timeo = 0;

	/* Wait for a max of 200ms for all the tx-completions to arrive. */
	do {
		while ((txcp = be_tx_compl_get(tx_cq))) {
			end_idx = AMAP_GET_BITS(struct amap_eth_tx_compl,
					wrb_index, txcp);
			be_tx_compl_process(adapter, end_idx);
			cmpl++;
		}
		if (cmpl) {
			be_cq_notify(adapter, tx_cq->id, false, cmpl);
			cmpl = 0;
		}

		if (atomic_read(&txq->used) == 0 || ++timeo > 200)
			break;

		mdelay(1);
	} while (true);

	if (atomic_read(&txq->used))
		dev_err(&adapter->pdev->dev, "%d pending tx-completions\n",
			atomic_read(&txq->used));
}

static void be_mcc_queues_destroy(struct be_adapter *adapter)
{
	struct be_queue_info *q;

	q = &adapter->mcc_obj.q;
	if (q->created)
		be_cmd_q_destroy(adapter, q, QTYPE_MCCQ);
	be_queue_free(adapter, q);

	q = &adapter->mcc_obj.cq;
	if (q->created)
		be_cmd_q_destroy(adapter, q, QTYPE_CQ);
	be_queue_free(adapter, q);
}

/* Must be called only after TX qs are created as MCC shares TX EQ */
static int be_mcc_queues_create(struct be_adapter *adapter)
{
	struct be_queue_info *q, *cq;

	/* Alloc MCC compl queue */
	cq = &adapter->mcc_obj.cq;
	if (be_queue_alloc(adapter, cq, MCC_CQ_LEN,
			sizeof(struct be_mcc_compl)))
		goto err;

	/* Ask BE to create MCC compl queue; share TX's eq */
	if (be_cmd_cq_create(adapter, cq, &adapter->be_eq.q, false, true, 0))
		goto mcc_cq_free;

	/* Alloc MCC queue */
	q = &adapter->mcc_obj.q;
	if (be_queue_alloc(adapter, q, MCC_Q_LEN, sizeof(struct be_mcc_wrb)))
		goto mcc_cq_destroy;

	/* Ask BE to create MCC queue */
	if (be_cmd_mccq_create(adapter, q, cq))
		goto mcc_q_free;

	return 0;

mcc_q_free:
	be_queue_free(adapter, q);
mcc_cq_destroy:
	be_cmd_q_destroy(adapter, cq, QTYPE_CQ);
mcc_cq_free:
	be_queue_free(adapter, cq);
err:
	return -1;
}

static void be_tx_queues_destroy(struct be_adapter *adapter)
{
	struct be_queue_info *q;

	q = &adapter->tx_obj.q;
	if (q->created)
		be_cmd_q_destroy(adapter, q, QTYPE_TXQ);
	be_queue_free(adapter, q);

	q = &adapter->tx_obj.cq;
	if (q->created)
		be_cmd_q_destroy(adapter, q, QTYPE_CQ);
	be_queue_free(adapter, q);

}

static int be_tx_queues_create(struct be_adapter *adapter)
{
	struct be_queue_info *eq, *q, *cq;

	eq = &adapter->be_eq.q;

	/* Alloc TX eth compl queue */
	cq = &adapter->tx_obj.cq;
	if (be_queue_alloc(adapter, cq, TX_CQ_LEN,
			sizeof(struct be_eth_tx_compl)))
		goto queue_alloc_failed;

	/* Ask BE to create Tx eth compl queue */
	if (be_cmd_cq_create(adapter, cq, eq, false, false, 3))
		goto tx_cq_free;

	/* Alloc TX eth queue */
	q = &adapter->tx_obj.q;
	if (be_queue_alloc(adapter, q, TX_Q_LEN, sizeof(struct be_eth_wrb)))
		goto tx_cq_destroy;

	/* Ask BE to create Tx eth queue */
	if (be_cmd_txq_create(adapter, q, cq))
		goto tx_q_free;
	return 0;

tx_q_free:
	be_queue_free(adapter, q);
tx_cq_destroy:
	be_cmd_q_destroy(adapter, cq, QTYPE_CQ);
tx_cq_free:
	be_queue_free(adapter, cq);
queue_alloc_failed:
	return -1;
}

static void be_rx_queues_destroy(struct be_adapter *adapter)
{
	struct be_queue_info *q;

	q = &adapter->rx_obj.q;
	if (q->created) {
		be_cmd_q_destroy(adapter, q, QTYPE_RXQ);
		be_rx_q_clean(adapter);
	}
	be_queue_free(adapter, q);

	q = &adapter->rx_obj.cq;
	if (q->created)
		be_cmd_q_destroy(adapter, q, QTYPE_CQ);
	be_queue_free(adapter, q);

	/* Clear any residual events */
	be_eq_clean(adapter, &adapter->be_eq);

	q = &adapter->be_eq.q;
	if (q->created)
		be_cmd_q_destroy(adapter, q, QTYPE_EQ);
	be_queue_free(adapter, q);
}

static int be_rx_queues_create(struct be_adapter *adapter)
{
	struct be_queue_info *eq, *q, *cq;
	int rc;

	adapter->big_page_size = (1 << get_order(rx_frag_size)) * PAGE_SIZE;
	adapter->be_eq.max_eqd = BE_MAX_EQD;
	adapter->be_eq.min_eqd = 0;
	adapter->be_eq.cur_eqd = 0;
	adapter->be_eq.enable_aic = true;

	/* Alloc Rx Event queue */
	eq = &adapter->be_eq.q;
	rc = be_queue_alloc(adapter, eq, EVNT_Q_LEN,
				sizeof(struct be_eq_entry));
	if (rc)
		return rc;

	/* Ask BE to create Rx Event queue */
	rc = be_cmd_eq_create(adapter, eq, adapter->be_eq.cur_eqd);
	if (rc)
		goto be_eq_free;

	/* Alloc RX eth compl queue */
	cq = &adapter->rx_obj.cq;
	rc = be_queue_alloc(adapter, cq, RX_CQ_LEN,
			sizeof(struct be_eth_rx_compl));
	if (rc)
		goto be_eq_destroy;

	/* Ask BE to create Rx eth compl queue */
	rc = be_cmd_cq_create(adapter, cq, eq, false, false, 3);
	if (rc)
		goto rx_cq_free;

	/* Alloc RX eth queue */
	q = &adapter->rx_obj.q;
	rc = be_queue_alloc(adapter, q, RX_Q_LEN, sizeof(struct be_eth_rx_d));
	if (rc)
		goto rx_cq_destroy;

	/* Ask BE to create Rx eth queue */
	rc = be_cmd_rxq_create(adapter, q, cq->id, rx_frag_size,
		BE_MAX_JUMBO_FRAME_SIZE, adapter->if_handle, false);
	if (rc)
		goto rx_q_free;

	return 0;
rx_q_free:
	be_queue_free(adapter, q);
rx_cq_destroy:
	be_cmd_q_destroy(adapter, cq, QTYPE_CQ);
rx_cq_free:
	be_queue_free(adapter, cq);
be_eq_destroy:
	be_cmd_q_destroy(adapter, eq, QTYPE_EQ);
be_eq_free:
	be_queue_free(adapter, eq);
	return rc;
}

/* There are 8 evt ids per func. Retruns the evt id's bit number */
static inline int be_evt_bit_get(struct be_adapter *adapter, u32 eq_id)
{
	return eq_id - 8 * be_pci_func(adapter);
}

static irqreturn_t be_intx(int irq, void *dev)
{
	struct be_adapter *adapter = dev;
	int isr;

	isr = ioread32(adapter->csr + CEV_ISR0_OFFSET +
			be_pci_func(adapter) * CEV_ISR_SIZE);
	if (!isr)
		return IRQ_NONE;

	napi_schedule(&adapter->napi);

	return IRQ_HANDLED;
}


static irqreturn_t be_msix(int irq, void *dev)
{
	struct be_adapter *adapter = dev;
	struct be_eq_obj *eq_obj = &adapter->be_eq;
	struct be_eq_entry *entry = queue_tail_node(&eq_obj->q);

	/* Clear the interrupt */
	be_eq_notify(adapter, eq_obj->q.id, false, true, 0);

	if (entry->evt)
		napi_schedule(&adapter->napi);
	else
		be_eq_notify(adapter, eq_obj->q.id, true, true, 0);

	return IRQ_HANDLED;
}

static inline bool do_gro(struct be_adapter *adapter,
			struct be_eth_rx_compl *rxcp)
{
	int err = AMAP_GET_BITS(struct amap_eth_rx_compl, err, rxcp);
	int tcp_frame = AMAP_GET_BITS(struct amap_eth_rx_compl, tcpf, rxcp);

	if (err)
		drvr_stats(adapter)->be_rxcp_err++;

	return (tcp_frame && !err) ? true : false;
}

int be_poll_rx(struct be_adapter *adapter, int budget)
{
	struct be_queue_info *rx_cq = &adapter->rx_obj.cq;
	struct be_eth_rx_compl *rxcp;
	u32 work;

	adapter->stats.drvr_stats.be_rx_polls++;

	for (work = 0; work < budget; work++) {
		rxcp = be_rx_compl_get(adapter);
		if (!rxcp)
			break;

		if (do_gro(adapter, rxcp))
			be_rx_compl_process_gro(adapter, rxcp);
		else
			be_rx_compl_process(adapter, rxcp);

		be_rx_compl_reset(rxcp);
	}

	/* Refill the queue */
	if (atomic_read(&adapter->rx_obj.q.used) < RX_FRAGS_REFILL_WM)
		be_post_rx_frags(adapter);

	if (work)
		be_cq_notify(adapter, rx_cq->id, true, work);

	return work;
}

/* As TX and MCC share the same EQ check for both TX and MCC completions.
 * For TX/MCC we don't honour budget; consume everything
 */
int be_poll_tx(struct be_adapter *adapter, int budget)
{
	struct be_tx_obj *tx_obj = &adapter->tx_obj;
	struct be_queue_info *tx_cq = &adapter->tx_obj.cq;
	struct be_queue_info *txq = &tx_obj->q;
	struct be_eth_tx_compl *txcp;
	u32 num_cmpl = 0;
	u16 end_idx;

	while ((txcp = be_tx_compl_get(tx_cq))) {
		end_idx = AMAP_GET_BITS(struct amap_eth_tx_compl,
					wrb_index, txcp);
		be_tx_compl_process(adapter, end_idx);
		num_cmpl++;
	}

	/* As Tx wrbs have been freed up, wake up netdev queue if
	 * it was stopped due to lack of tx wrbs.
	 */
	if (netif_queue_stopped(adapter->netdev) &&
			atomic_read(&txq->used) < txq->len / 2) {
		netif_wake_queue(adapter->netdev);
	}

	drvr_stats(adapter)->be_tx_compl += num_cmpl;

	if (num_cmpl)
		be_cq_notify(adapter, tx_cq->id, true, num_cmpl);

	return num_cmpl;
}

int be_poll(struct napi_struct *napi, int budget)
{
	struct be_adapter *adapter =
				container_of(napi, struct be_adapter, napi);
	struct be_eq_obj *eq_obj = &adapter->be_eq;
	u16 num = 0;
	u32 tx_work, rx_work;

	while (event_get(eq_obj))
		num++;

	tx_work = be_poll_tx(adapter, budget);
	rx_work = be_poll_rx(adapter, budget);
	be_process_mcc(adapter);

	drvr_stats(adapter)->be_num_events += num;

	/* All consumed */
	if (rx_work < budget) {
		napi_complete(napi);
		be_eq_notify(adapter, eq_obj->q.id, true, false, num);
	} else
		be_eq_notify(adapter, eq_obj->q.id, false, false, num);

	return rx_work;
}

static void be_worker(struct work_struct *work)
{
	struct be_adapter *adapter =
		container_of(work, struct be_adapter, work.work);
	int status;

	/* Get Stats */
	status = be_cmd_get_stats(adapter, &adapter->stats.cmd);
	if (!status)
		netdev_stats_update(adapter);

	/* Set EQ delay */
	be_eqd_update(adapter);

	be_tx_rate_update(adapter);
	be_rx_rate_update(adapter);

	if (adapter->rx_post_starved) {
		adapter->rx_post_starved = false;
		be_post_rx_frags(adapter);
	}

	schedule_delayed_work(&adapter->work, msecs_to_jiffies(1000));
}

static void be_msix_disable(struct be_adapter *adapter)
{
	if (adapter->msix_enabled) {
		pci_disable_msix(adapter->pdev);
		adapter->msix_enabled = false;
	}
}

static void be_msix_enable(struct be_adapter *adapter)
{
	int i, status;

	for (i = 0; i < BE_NUM_MSIX_VECTORS; i++)
		adapter->msix_entries[i].entry = i;

	status = pci_enable_msix(adapter->pdev, adapter->msix_entries,
		BE_NUM_MSIX_VECTORS);
	if (status == 0)
		adapter->msix_enabled = true;
	return;
}

static inline int be_msix_vec_get(struct be_adapter *adapter, u32 eq_id)
{
	return adapter->msix_entries[
			be_evt_bit_get(adapter, eq_id)].vector;
}

static int be_request_irq(struct be_adapter *adapter,
		struct be_eq_obj *eq_obj,
		void *handler)
{
	struct net_device *netdev = adapter->netdev;
	int vec;

	sprintf(eq_obj->desc, "%s", netdev->name);
	vec = be_msix_vec_get(adapter, eq_obj->q.id);
	return request_irq(vec, handler, 0, eq_obj->desc, adapter);
}

static void be_free_irq(struct be_adapter *adapter, struct be_eq_obj *eq_obj)
{
	int vec = be_msix_vec_get(adapter, eq_obj->q.id);
	free_irq(vec, adapter);
}

static int be_msix_register(struct be_adapter *adapter)
{
	int status;

	status = be_request_irq(adapter, &adapter->be_eq, be_msix);
	if (status)
		goto err;

	return 0;

err:
	dev_warn(&adapter->pdev->dev,
		"MSIX Request IRQ failed - err %d\n", status);
	be_msix_disable(adapter);
	return status;
}

static int be_irq_register(struct be_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	int status;

	if (adapter->msix_enabled) {
		status = be_msix_register(adapter);
		if (status == 0)
			goto done;
	}

	/* INTx */
	netdev->irq = adapter->pdev->irq;
	status = request_irq(netdev->irq, be_intx, IRQF_SHARED, netdev->name,
			adapter);
	if (status) {
		dev_err(&adapter->pdev->dev,
			"INTx request IRQ failed - err %d\n", status);
		return status;
	}
done:
	adapter->isr_registered = true;
	return 0;
}

static void be_irq_unregister(struct be_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;

	if (!adapter->isr_registered)
		return;

	/* INTx */
	if (!adapter->msix_enabled) {
		free_irq(netdev->irq, adapter);
		goto done;
	}

	/* MSIx */
	be_free_irq(adapter, &adapter->be_eq);
done:
	adapter->isr_registered = false;
	return;
}

static int be_open(struct net_device *netdev)
{
	struct be_adapter *adapter = netdev_priv(netdev);
	struct be_eq_obj *be_eq = &adapter->be_eq;
	bool link_up;
	int status;
	u8 mac_speed;
	u16 link_speed;

	/* First time posting */
	be_post_rx_frags(adapter);

	napi_enable(&adapter->napi);

	be_irq_register(adapter);

	be_intr_set(adapter, true);

	/* The evt queues are created in unarmed state; arm them */
	be_eq_notify(adapter, be_eq->q.id, true, false, 0);

	/* Rx compl queue may be in unarmed state; rearm it */
	be_cq_notify(adapter, adapter->rx_obj.cq.id, true, 0);

	/* Now that interrupts are on we can process async mcc */
	be_async_mcc_enable(adapter);

	status = be_cmd_link_status_query(adapter, &link_up, &mac_speed,
			&link_speed);
	if (status)
		goto ret_sts;
	be_link_status_update(adapter, link_up);

	status = be_vid_config(adapter);
	if (status)
		goto ret_sts;

	status = be_cmd_set_flow_control(adapter,
					adapter->tx_fc, adapter->rx_fc);
	if (status)
		goto ret_sts;

	schedule_delayed_work(&adapter->work, msecs_to_jiffies(100));
ret_sts:
	return status;
}

static int be_setup_wol(struct be_adapter *adapter, bool enable)
{
	struct be_dma_mem cmd;
	int status = 0;
	u8 mac[ETH_ALEN];

	memset(mac, 0, ETH_ALEN);

	cmd.size = sizeof(struct be_cmd_req_acpi_wol_magic_config);
	cmd.va = pci_alloc_consistent(adapter->pdev, cmd.size, &cmd.dma);
	if (cmd.va == NULL)
		return -1;
	memset(cmd.va, 0, cmd.size);

	if (enable) {
		status = pci_write_config_dword(adapter->pdev,
			PCICFG_PM_CONTROL_OFFSET, PCICFG_PM_CONTROL_MASK);
		if (status) {
			dev_err(&adapter->pdev->dev,
				"Could not enable Wake-on-lan \n");
			pci_free_consistent(adapter->pdev, cmd.size, cmd.va,
					cmd.dma);
			return status;
		}
		status = be_cmd_enable_magic_wol(adapter,
				adapter->netdev->dev_addr, &cmd);
		pci_enable_wake(adapter->pdev, PCI_D3hot, 1);
		pci_enable_wake(adapter->pdev, PCI_D3cold, 1);
	} else {
		status = be_cmd_enable_magic_wol(adapter, mac, &cmd);
		pci_enable_wake(adapter->pdev, PCI_D3hot, 0);
		pci_enable_wake(adapter->pdev, PCI_D3cold, 0);
	}

	pci_free_consistent(adapter->pdev, cmd.size, cmd.va, cmd.dma);
	return status;
}

static int be_setup(struct be_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	u32 cap_flags, en_flags;
	int status;

	cap_flags = BE_IF_FLAGS_UNTAGGED | BE_IF_FLAGS_BROADCAST |
			BE_IF_FLAGS_MCAST_PROMISCUOUS |
			BE_IF_FLAGS_PROMISCUOUS |
			BE_IF_FLAGS_PASS_L3L4_ERRORS;
	en_flags = BE_IF_FLAGS_UNTAGGED | BE_IF_FLAGS_BROADCAST |
			BE_IF_FLAGS_PASS_L3L4_ERRORS;

	status = be_cmd_if_create(adapter, cap_flags, en_flags,
			netdev->dev_addr, false/* pmac_invalid */,
			&adapter->if_handle, &adapter->pmac_id);
	if (status != 0)
		goto do_none;

	status = be_rx_queues_create(adapter);
	if (status != 0)
		goto tx_qs_destroy;

	status = be_tx_queues_create(adapter);
	if (status != 0)
		goto if_destroy;

	status = be_mcc_queues_create(adapter);
	if (status != 0)
		goto rx_qs_destroy;

	adapter->link_speed = -1;

	return 0;

rx_qs_destroy:
	be_rx_queues_destroy(adapter);
tx_qs_destroy:
	be_tx_queues_destroy(adapter);
if_destroy:
	be_cmd_if_destroy(adapter, adapter->if_handle);
do_none:
	return status;
}

static int be_clear(struct be_adapter *adapter)
{
	be_mcc_queues_destroy(adapter);
	be_rx_queues_destroy(adapter);
	be_tx_queues_destroy(adapter);

	be_cmd_if_destroy(adapter, adapter->if_handle);

	return 0;
}

static int be_close(struct net_device *netdev)
{
	struct be_adapter *adapter = netdev_priv(netdev);
	struct be_eq_obj *be_eq = &adapter->be_eq;
	int vec;

	cancel_delayed_work_sync(&adapter->work);

	be_async_mcc_disable(adapter);

	netif_stop_queue(netdev);
	netif_carrier_off(netdev);
	adapter->link_up = false;

	be_intr_set(adapter, false);

	if (adapter->msix_enabled) {
		vec = be_msix_vec_get(adapter, be_eq->q.id);
		synchronize_irq(vec);
	} else {
		synchronize_irq(netdev->irq);
	}
	be_irq_unregister(adapter);

	napi_disable(&adapter->napi);

	/* Wait for all pending tx completions to arrive so that
	 * all tx skbs are freed.
	 */
	be_tx_compl_clean(adapter);

	return 0;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void be_netpoll(struct net_device *netdev)
{
	struct be_adapter *adapter = netdev_priv(netdev);
	struct be_eq_obj *eq_obj = &adapter->be_eq;
	struct be_eq_entry *entry = queue_tail_node(&eq_obj->q);

	/* Clear the interrupt */
	be_eq_notify(adapter, eq_obj->q.id, false, true, 0);

	if (entry->evt)
		napi_schedule(&adapter->napi);
	else
		be_eq_notify(adapter, eq_obj->q.id, true, true, 0);

	return;
}
#endif

#define FW_FILE_HDR_SIGN 	"ServerEngines Corp. "
char flash_cookie[2][16] =	{"*** SE FLAS",
				"H DIRECTORY *** "};

static bool be_flash_redboot(struct be_adapter *adapter,
			const u8 *p, u32 img_start, int image_size,
			int hdr_size)
{
	u32 crc_offset;
	u8 flashed_crc[4];
	int status;

	crc_offset = hdr_size + img_start + image_size - 4;

	p += crc_offset;

	status = be_cmd_get_flash_crc(adapter, flashed_crc,
			(img_start + image_size - 4));
	if (status) {
		dev_err(&adapter->pdev->dev,
		"could not get crc from flash, not flashing redboot\n");
		return false;
	}

	/*update redboot only if crc does not match*/
	if (!memcmp(flashed_crc, p, 4))
		return false;
	else
		return true;
}

static int be_flash_data(struct be_adapter *adapter,
			const struct firmware *fw,
			struct be_dma_mem *flash_cmd, int num_of_images)

{
	int status = 0, i, filehdr_size = 0;
	u32 total_bytes = 0, flash_op;
	int num_bytes;
	const u8 *p = fw->data;
	struct be_cmd_write_flashrom *req = flash_cmd->va;
	struct flash_comp *pflashcomp;

	struct flash_comp gen3_flash_types[8] = {
		{ FLASH_iSCSI_PRIMARY_IMAGE_START_g3, IMG_TYPE_ISCSI_ACTIVE,
			FLASH_IMAGE_MAX_SIZE_g3},
		{ FLASH_REDBOOT_START_g3, IMG_TYPE_REDBOOT,
			FLASH_REDBOOT_IMAGE_MAX_SIZE_g3},
		{ FLASH_iSCSI_BIOS_START_g3, IMG_TYPE_BIOS,
			FLASH_BIOS_IMAGE_MAX_SIZE_g3},
		{ FLASH_PXE_BIOS_START_g3, IMG_TYPE_PXE_BIOS,
			FLASH_BIOS_IMAGE_MAX_SIZE_g3},
		{ FLASH_FCoE_BIOS_START_g3, IMG_TYPE_FCOE_BIOS,
			FLASH_BIOS_IMAGE_MAX_SIZE_g3},
		{ FLASH_iSCSI_BACKUP_IMAGE_START_g3, IMG_TYPE_ISCSI_BACKUP,
			FLASH_IMAGE_MAX_SIZE_g3},
		{ FLASH_FCoE_PRIMARY_IMAGE_START_g3, IMG_TYPE_FCOE_FW_ACTIVE,
			FLASH_IMAGE_MAX_SIZE_g3},
		{ FLASH_FCoE_BACKUP_IMAGE_START_g3, IMG_TYPE_FCOE_FW_BACKUP,
			FLASH_IMAGE_MAX_SIZE_g3}
	};
	struct flash_comp gen2_flash_types[8] = {
		{ FLASH_iSCSI_PRIMARY_IMAGE_START_g2, IMG_TYPE_ISCSI_ACTIVE,
			FLASH_IMAGE_MAX_SIZE_g2},
		{ FLASH_REDBOOT_START_g2, IMG_TYPE_REDBOOT,
			FLASH_REDBOOT_IMAGE_MAX_SIZE_g2},
		{ FLASH_iSCSI_BIOS_START_g2, IMG_TYPE_BIOS,
			FLASH_BIOS_IMAGE_MAX_SIZE_g2},
		{ FLASH_PXE_BIOS_START_g2, IMG_TYPE_PXE_BIOS,
			FLASH_BIOS_IMAGE_MAX_SIZE_g2},
		{ FLASH_FCoE_BIOS_START_g2, IMG_TYPE_FCOE_BIOS,
			FLASH_BIOS_IMAGE_MAX_SIZE_g2},
		{ FLASH_iSCSI_BACKUP_IMAGE_START_g2, IMG_TYPE_ISCSI_BACKUP,
			FLASH_IMAGE_MAX_SIZE_g2},
		{ FLASH_FCoE_PRIMARY_IMAGE_START_g2, IMG_TYPE_FCOE_FW_ACTIVE,
			FLASH_IMAGE_MAX_SIZE_g2},
		{ FLASH_FCoE_BACKUP_IMAGE_START_g2, IMG_TYPE_FCOE_FW_BACKUP,
			 FLASH_IMAGE_MAX_SIZE_g2}
	};

	if (adapter->generation == BE_GEN3) {
		pflashcomp = gen3_flash_types;
		filehdr_size = sizeof(struct flash_file_hdr_g3);
	} else {
		pflashcomp = gen2_flash_types;
		filehdr_size = sizeof(struct flash_file_hdr_g2);
	}
	for (i = 0; i < 8; i++) {
		if ((pflashcomp[i].optype == IMG_TYPE_REDBOOT) &&
			(!be_flash_redboot(adapter, fw->data,
			 pflashcomp[i].offset, pflashcomp[i].size,
			 filehdr_size)))
			continue;
		p = fw->data;
		p += filehdr_size + pflashcomp[i].offset
			+ (num_of_images * sizeof(struct image_hdr));
	if (p + pflashcomp[i].size > fw->data + fw->size)
		return -1;
	total_bytes = pflashcomp[i].size;
		while (total_bytes) {
			if (total_bytes > 32*1024)
				num_bytes = 32*1024;
			else
				num_bytes = total_bytes;
			total_bytes -= num_bytes;

			if (!total_bytes)
				flash_op = FLASHROM_OPER_FLASH;
			else
				flash_op = FLASHROM_OPER_SAVE;
			memcpy(req->params.data_buf, p, num_bytes);
			p += num_bytes;
			status = be_cmd_write_flashrom(adapter, flash_cmd,
				pflashcomp[i].optype, flash_op, num_bytes);
			if (status) {
				dev_err(&adapter->pdev->dev,
					"cmd to write to flash rom failed.\n");
				return -1;
			}
			yield();
		}
	}
	return 0;
}

static int get_ufigen_type(struct flash_file_hdr_g2 *fhdr)
{
	if (fhdr == NULL)
		return 0;
	if (fhdr->build[0] == '3')
		return BE_GEN3;
	else if (fhdr->build[0] == '2')
		return BE_GEN2;
	else
		return 0;
}

#define ETHTOOL_FLASH_MAX_FILENAME 128
int be_load_fw(struct be_adapter *adapter, u8 *func)
{
	char fw_file[ETHTOOL_FLASH_MAX_FILENAME];
	const struct firmware *fw;
	struct flash_file_hdr_g2 *fhdr;
	struct flash_file_hdr_g3 *fhdr3;
	struct image_hdr *img_hdr_ptr = NULL;
	struct be_dma_mem flash_cmd;
	int status, i = 0;
	const u8 *p;
	char fw_ver[FW_VER_LEN];
	char fw_cfg;

	status = be_cmd_get_fw_ver(adapter, fw_ver);
	if (status)
		return status;

	fw_cfg = *(fw_ver + 2);
	if (fw_cfg == '0')
		fw_cfg = '1';
	strcpy(fw_file, func);

	status = request_firmware(&fw, fw_file, &adapter->pdev->dev);
	if (status)
		goto fw_exit;

	p = fw->data;
	fhdr = (struct flash_file_hdr_g2 *) p;
	dev_info(&adapter->pdev->dev, "Flashing firmware file %s\n", fw_file);

	flash_cmd.size = sizeof(struct be_cmd_write_flashrom) + 32*1024;
	flash_cmd.va = pci_alloc_consistent(adapter->pdev, flash_cmd.size,
					&flash_cmd.dma);
	if (!flash_cmd.va) {
		status = -ENOMEM;
		dev_err(&adapter->pdev->dev,
			"Memory allocation failure while flashing\n");
		goto fw_exit;
	}

	if ((adapter->generation == BE_GEN3) &&
			(get_ufigen_type(fhdr) == BE_GEN3)) {
		fhdr3 = (struct flash_file_hdr_g3 *) fw->data;
		for (i = 0; i < fhdr3->num_imgs; i++) {
			img_hdr_ptr = (struct image_hdr *) (fw->data +
					(sizeof(struct flash_file_hdr_g3) +
					i * sizeof(struct image_hdr)));
			if (img_hdr_ptr->imageid == 1) {
				status = be_flash_data(adapter, fw,
						&flash_cmd, fhdr3->num_imgs);
			}

		}
	} else if ((adapter->generation == BE_GEN2) &&
			(get_ufigen_type(fhdr) == BE_GEN2)) {
		status = be_flash_data(adapter, fw, &flash_cmd, 0);
	} else {
		dev_err(&adapter->pdev->dev,
			"UFI and Interface are not compatible for flashing\n");
		status = -1;
	}

	pci_free_consistent(adapter->pdev, flash_cmd.size, flash_cmd.va,
				flash_cmd.dma);
	if (status) {
		dev_err(&adapter->pdev->dev, "Firmware load error\n");
		goto fw_exit;
	}

	dev_info(&adapter->pdev->dev, "Firmware flashed successfully\n");

fw_exit:
	release_firmware(fw);
	return status;
}

static struct net_device_ops be_netdev_ops = {
	.ndo_open		= be_open,
	.ndo_stop		= be_close,
	.ndo_start_xmit		= be_xmit,
	.ndo_get_stats		= be_get_stats,
	.ndo_set_rx_mode	= be_set_multicast_list,
	.ndo_set_mac_address	= be_mac_addr_set,
	.ndo_change_mtu		= be_change_mtu,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_vlan_rx_register	= be_vlan_register,
	.ndo_vlan_rx_add_vid	= be_vlan_add_vid,
	.ndo_vlan_rx_kill_vid	= be_vlan_rem_vid,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= be_netpoll,
#endif
};

static void be_netdev_init(struct net_device *netdev)
{
	struct be_adapter *adapter = netdev_priv(netdev);

	netdev->features |= NETIF_F_SG | NETIF_F_HW_VLAN_RX | NETIF_F_TSO |
		NETIF_F_HW_VLAN_TX | NETIF_F_HW_VLAN_FILTER | NETIF_F_HW_CSUM |
		NETIF_F_GRO;

	netdev->flags |= IFF_MULTICAST;

	adapter->rx_csum = true;

	/* Default settings for Rx and Tx flow control */
	adapter->rx_fc = true;
	adapter->tx_fc = true;

	BE_SET_NETDEV_OPS(netdev, &be_netdev_ops);

	SET_ETHTOOL_OPS(netdev, &be_ethtool_ops);

	netif_napi_add(netdev, &adapter->napi, be_poll,
		BE_NAPI_WEIGHT);

	netif_carrier_off(netdev);
	netif_stop_queue(netdev);
}

static void be_unmap_pci_bars(struct be_adapter *adapter)
{
	if (adapter->csr)
		iounmap(adapter->csr);
	if (adapter->db)
		iounmap(adapter->db);
	if (adapter->pcicfg)
		iounmap(adapter->pcicfg);
}

static int be_map_pci_bars(struct be_adapter *adapter)
{
	u8 __iomem *addr;
	int pcicfg_reg;

	addr = ioremap_nocache(pci_resource_start(adapter->pdev, 2),
			pci_resource_len(adapter->pdev, 2));
	if (addr == NULL)
		return -ENOMEM;
	adapter->csr = addr;

	addr = ioremap_nocache(pci_resource_start(adapter->pdev, 4),
			128 * 1024);
	if (addr == NULL)
		goto pci_map_err;
	adapter->db = addr;

	if (adapter->generation == BE_GEN2)
		pcicfg_reg = 1;
	else
		pcicfg_reg = 0;

	addr = ioremap_nocache(pci_resource_start(adapter->pdev, pcicfg_reg),
			pci_resource_len(adapter->pdev, pcicfg_reg));
	if (addr == NULL)
		goto pci_map_err;
	adapter->pcicfg = addr;

	return 0;
pci_map_err:
	be_unmap_pci_bars(adapter);
	return -ENOMEM;
}


static void be_ctrl_cleanup(struct be_adapter *adapter)
{
	struct be_dma_mem *mem = &adapter->mbox_mem_alloced;

	be_unmap_pci_bars(adapter);

	if (mem->va)
		pci_free_consistent(adapter->pdev, mem->size,
			mem->va, mem->dma);

	mem = &adapter->mc_cmd_mem;
	if (mem->va)
		pci_free_consistent(adapter->pdev, mem->size,
			mem->va, mem->dma);
}

static int be_ctrl_init(struct be_adapter *adapter)
{
	struct be_dma_mem *mbox_mem_alloc = &adapter->mbox_mem_alloced;
	struct be_dma_mem *mbox_mem_align = &adapter->mbox_mem;
	struct be_dma_mem *mc_cmd_mem = &adapter->mc_cmd_mem;
	int status;

	status = be_map_pci_bars(adapter);
	if (status)
		goto done;

	mbox_mem_alloc->size = sizeof(struct be_mcc_mailbox) + 16;
	mbox_mem_alloc->va = pci_alloc_consistent(adapter->pdev,
				mbox_mem_alloc->size, &mbox_mem_alloc->dma);
	if (!mbox_mem_alloc->va) {
		status = -ENOMEM;
		goto unmap_pci_bars;
	}

	mbox_mem_align->size = sizeof(struct be_mcc_mailbox);
	mbox_mem_align->va = PTR_ALIGN(mbox_mem_alloc->va, 16);
	mbox_mem_align->dma = PTR_ALIGN(mbox_mem_alloc->dma, 16);
	memset(mbox_mem_align->va, 0, sizeof(struct be_mcc_mailbox));

	mc_cmd_mem->size = sizeof(struct be_cmd_req_mcast_mac_config);
	mc_cmd_mem->va = pci_alloc_consistent(adapter->pdev, mc_cmd_mem->size,
			&mc_cmd_mem->dma);
	if (mc_cmd_mem->va == NULL) {
		status = -ENOMEM;
		goto free_mbox;
	}
	memset(mc_cmd_mem->va, 0, mc_cmd_mem->size);

	spin_lock_init(&adapter->mbox_lock);
	spin_lock_init(&adapter->mcc_lock);
	spin_lock_init(&adapter->mcc_cq_lock);

	return 0;

free_mbox:
	pci_free_consistent(adapter->pdev, mbox_mem_alloc->size,
		mbox_mem_alloc->va, mbox_mem_alloc->dma);

unmap_pci_bars:
	be_unmap_pci_bars(adapter);

done:
	return status;
}

static void be_stats_cleanup(struct be_adapter *adapter)
{
	struct be_stats_obj *stats = &adapter->stats;
	struct be_dma_mem *cmd = &stats->cmd;

	if (cmd->va)
		pci_free_consistent(adapter->pdev, cmd->size,
			cmd->va, cmd->dma);
}

static int be_stats_init(struct be_adapter *adapter)
{
	struct be_stats_obj *stats = &adapter->stats;
	struct be_dma_mem *cmd = &stats->cmd;

	cmd->size = sizeof(struct be_cmd_req_get_stats);
	cmd->va = pci_alloc_consistent(adapter->pdev, cmd->size, &cmd->dma);
	if (cmd->va == NULL)
		return -1;
	memset(cmd->va, 0, cmd->size);
	return 0;
}

static ssize_t flash_fw_store(struct class_device *cdev, const char *buf,
			      size_t len)
{
	struct be_adapter *adapter =
		netdev_priv(container_of(cdev, struct net_device, class_dev));
	char file_name[ETHTOOL_FLASH_MAX_FILENAME];
	int status;

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	file_name[ETHTOOL_FLASH_MAX_FILENAME - 1] = 0;
	strncpy(file_name, buf, (ETHTOOL_FLASH_MAX_FILENAME - 1));

	/* Handle situation if filename comes with newline */
	if (file_name[strlen(file_name) - 1] == '\n')
		file_name[strlen(file_name) - 1] = '\0';

	status = be_load_fw(adapter, file_name);
	if (!status)
		return len;
	else
		return status;
}

static CLASS_DEVICE_ATTR(flash_fw, S_IWUSR, NULL, flash_fw_store);

static struct attribute *benet_attrs[] = {
	&class_device_attr_flash_fw.attr,
	NULL,
};

static struct attribute_group benet_attr_group = {.attrs = benet_attrs };

static void __devexit be_remove(struct pci_dev *pdev)
{
	struct be_adapter *adapter = pci_get_drvdata(pdev);
	if (!adapter)
		return;

	sysfs_remove_group(&adapter->netdev->class_dev.kobj, &benet_attr_group);

	unregister_netdev(adapter->netdev);

	be_clear(adapter);

	be_stats_cleanup(adapter);

	be_ctrl_cleanup(adapter);

	be_msix_disable(adapter);

	pci_set_drvdata(pdev, NULL);
	pci_release_regions(pdev);
	pci_disable_device(pdev);

	free_netdev(adapter->netdev);
}

static int be_get_config(struct be_adapter *adapter)
{
	int status;
	u8 mac[ETH_ALEN];

	status = be_cmd_get_fw_ver(adapter, adapter->fw_ver);
	if (status)
		return status;

	memset(mac, 0, ETH_ALEN);
	status = be_cmd_query_fw_cfg(adapter, &adapter->port_num,
			&adapter->cap);
	if (status)
		return status;

	status = be_cmd_mac_addr_query(adapter, mac,
			MAC_ADDRESS_TYPE_NETWORK, true /*permanent */, 0);
	if (status)
		return status;
	memcpy(adapter->netdev->dev_addr, mac, ETH_ALEN);

	return 0;
}

static int __devinit be_probe(struct pci_dev *pdev,
			const struct pci_device_id *pdev_id)
{
	int status = 0;
	struct be_adapter *adapter;
	struct net_device *netdev;

	status = pci_enable_device(pdev);
	if (status)
		goto do_none;

	status = pci_request_regions(pdev, DRV_NAME);
	if (status)
		goto disable_dev;
	pci_set_master(pdev);

	netdev = alloc_etherdev(sizeof(struct be_adapter));
	if (netdev == NULL) {
		status = -ENOMEM;
		goto rel_reg;
	}
	adapter = netdev_priv(netdev);

	switch (pdev->device) {
	case BE_DEVICE_ID1:
	case OC_DEVICE_ID1:
		adapter->generation = BE_GEN2;
		break;
	case BE_DEVICE_ID2:
	case OC_DEVICE_ID2:
		adapter->generation = BE_GEN3;
		break;
	default:
		adapter->generation = 0;
	}

	adapter->pdev = pdev;
	pci_set_drvdata(pdev, adapter);
	adapter->netdev = netdev;
	be_netdev_init(netdev);
	SET_NETDEV_DEV(netdev, &pdev->dev);

	be_msix_enable(adapter);

	status = pci_set_dma_mask(pdev, DMA_BIT_MASK(64));
	if (!status) {
		netdev->features |= NETIF_F_HIGHDMA;
	} else {
		status = pci_set_dma_mask(pdev, DMA_BIT_MASK(32));
		if (status) {
			dev_err(&pdev->dev, "Could not set PCI DMA Mask\n");
			goto free_netdev;
		}
	}

	status = be_ctrl_init(adapter);
	if (status)
		goto free_netdev;

	/* sync up with fw's ready state */
	status = be_cmd_POST(adapter);
	if (status)
		goto ctrl_clean;

	/* tell fw we're ready to fire cmds */
	status = be_cmd_fw_init(adapter);
	if (status)
		goto ctrl_clean;

	status = be_cmd_reset_function(adapter);
	if (status)
		goto ctrl_clean;

	status = be_stats_init(adapter);
	if (status)
		goto ctrl_clean;

	status = be_get_config(adapter);
	if (status)
		goto stats_clean;

	INIT_DELAYED_WORK(&adapter->work, be_worker);

	status = be_setup(adapter);
	if (status)
		goto stats_clean;

	status = register_netdev(netdev);
	if (status != 0)
		goto unsetup;

	if (sysfs_create_group(&adapter->netdev->class_dev.kobj,
			       &benet_attr_group))
		dev_err(&pdev->dev, "Could not create sysfs group\n");

	dev_info(&pdev->dev, "%s - %s\n", netdev->name, nic_name(pdev));
	return 0;

unsetup:
	be_clear(adapter);
stats_clean:
	be_stats_cleanup(adapter);
ctrl_clean:
	be_ctrl_cleanup(adapter);
free_netdev:
	be_msix_disable(adapter);
	free_netdev(adapter->netdev);
	pci_set_drvdata(pdev, NULL);
rel_reg:
	pci_release_regions(pdev);
disable_dev:
	pci_disable_device(pdev);
do_none:
	dev_err(&pdev->dev, "%s initialization failed\n", nic_name(pdev));
	return status;
}

static int be_suspend(struct pci_dev *pdev, pm_message_t state)
{
	struct be_adapter *adapter = pci_get_drvdata(pdev);
	struct net_device *netdev =  adapter->netdev;

	if (adapter->wol)
		be_setup_wol(adapter, true);

	netif_device_detach(netdev);
	if (netif_running(netdev)) {
		rtnl_lock();
		be_close(netdev);
		rtnl_unlock();
	}
	be_cmd_get_flow_control(adapter, &adapter->tx_fc, &adapter->rx_fc);
	be_clear(adapter);

	pci_save_state(pdev);
	pci_disable_device(pdev);
	pci_set_power_state(pdev, pci_choose_state(pdev, state));
	return 0;
}

static int be_resume(struct pci_dev *pdev)
{
	int status = 0;
	struct be_adapter *adapter = pci_get_drvdata(pdev);
	struct net_device *netdev =  adapter->netdev;

	netif_device_detach(netdev);

	status = pci_enable_device(pdev);
	if (status)
		return status;

	pci_set_power_state(pdev, 0);
	pci_restore_state(pdev);

	/* tell fw we're ready to fire cmds */
	status = be_cmd_fw_init(adapter);
	if (status)
		return status;

	be_setup(adapter);

	if (netif_running(netdev)) {
		rtnl_lock();
		be_open(netdev);
		rtnl_unlock();
	}
	netif_device_attach(netdev);

	if (adapter->wol)
		be_setup_wol(adapter, false);
	return 0;
}

static struct pci_driver be_driver = {
	.name = DRV_NAME,
	.id_table = be_dev_ids,
	.probe = be_probe,
	.remove = be_remove,
	.suspend = be_suspend,
	.resume = be_resume
};

static int __init be_init_module(void)
{
	if (lro != 1) {
		printk(KERN_WARNING DRV_NAME
		       "The driver doesn't support LRO anymore. "
		       "Don't use this parameter!\n");
	}

	if (rx_frag_size != 8192 && rx_frag_size != 4096
		&& rx_frag_size != 2048) {
		printk(KERN_WARNING DRV_NAME
			" : Module param rx_frag_size must be 2048/4096/8192."
			" Using 2048\n");
		rx_frag_size = 2048;
	}

	return pci_register_driver(&be_driver);
}
module_init(be_init_module);

static void __exit be_exit_module(void)
{
	pci_unregister_driver(&be_driver);
}
module_exit(be_exit_module);
