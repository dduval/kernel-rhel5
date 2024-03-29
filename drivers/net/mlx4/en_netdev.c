/*
 * Copyright (c) 2007 Mellanox Technologies. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#include <linux/etherdevice.h>
#include <linux/tcp.h>
#include <linux/if_vlan.h>
#include <linux/delay.h>

#include <linux/mlx4/driver.h>
#include <linux/mlx4/device.h>
#include <linux/mlx4/cmd.h>
#include <linux/mlx4/cq.h>

#include "mlx4_en.h"
#include "en_port.h"


static void mlx4_en_vlan_rx_register(struct net_device *dev, struct vlan_group *grp)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	struct mlx4_en_dev *mdev = priv->mdev;
	int err;

	en_dbg(HW, priv, "Regsitering VLAN group:%p\n", grp);
	priv->vlgrp = grp;

	mutex_lock(&mdev->state_lock);
	if (mdev->device_up && priv->port_up) {
		err = mlx4_SET_VLAN_FLTR(mdev->dev, priv->port, grp);
		if (err)
			en_err(priv, "Failed configuring VLAN filter\n");
	}
	mutex_unlock(&mdev->state_lock);
}

static void mlx4_en_vlan_rx_add_vid(struct net_device *dev, unsigned short vid)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	struct mlx4_en_dev *mdev = priv->mdev;
	int err;
#ifndef HAVE_NETDEV_VLAN_FEATURES
	struct net_device *vdev;
#endif

	if (!priv->vlgrp)
		return;

	en_dbg(HW, priv, "adding VLAN:%d (vlgrp entry:%p)\n",
	       vid, vlan_group_get_device(priv->vlgrp, vid));

	/* Add VID to port VLAN filter */
	mutex_lock(&mdev->state_lock);
	if (mdev->device_up && priv->port_up) {
		err = mlx4_SET_VLAN_FLTR(mdev->dev, priv->port, priv->vlgrp);
		if (err)
			en_err(priv, "Failed configuring VLAN filter\n");
	}
	mutex_unlock(&mdev->state_lock);

#ifndef HAVE_NETDEV_VLAN_FEATURES
	vdev = vlan_group_get_device(priv->vlgrp, vid);
	vdev->features |= dev->features;
	vdev->features |= NETIF_F_LLTX;
	vlan_group_set_device(priv->vlgrp, vid, vdev);
#endif
}

static void mlx4_en_vlan_rx_kill_vid(struct net_device *dev, unsigned short vid)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	struct mlx4_en_dev *mdev = priv->mdev;
	int err;

	if (!priv->vlgrp)
		return;

	en_dbg(HW, priv, "Killing VID:%d (vlgrp:%p vlgrp entry:%p)\n",
	       vid, priv->vlgrp, vlan_group_get_device(priv->vlgrp, vid));
	vlan_group_set_device(priv->vlgrp, vid, NULL);

	/* Remove VID from port VLAN filter */
	mutex_lock(&mdev->state_lock);
	if (mdev->device_up && priv->port_up) {
		err = mlx4_SET_VLAN_FLTR(mdev->dev, priv->port, priv->vlgrp);
		if (err)
			en_err(priv, "Failed configuring VLAN filter\n");
	}
	mutex_unlock(&mdev->state_lock);
}

u64 mlx4_en_mac_to_u64(u8 *addr)
{
	u64 mac = 0;
	int i;

	for (i = 0; i < ETH_ALEN; i++) {
		mac <<= 8;
		mac |= addr[i];
	}
	return mac;
}

static int mlx4_en_set_mac(struct net_device *dev, void *addr)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	struct mlx4_en_dev *mdev = priv->mdev;
	struct sockaddr *saddr = addr;

	if (!is_valid_ether_addr(saddr->sa_data))
		return -EADDRNOTAVAIL;

	memcpy(dev->dev_addr, saddr->sa_data, ETH_ALEN);
	priv->mac = mlx4_en_mac_to_u64(dev->dev_addr);
	queue_work(mdev->workqueue, &priv->mac_task);
	return 0;
}

static void mlx4_en_do_set_mac(struct work_struct *work)
{
	struct mlx4_en_priv *priv = container_of(work, struct mlx4_en_priv,
						 mac_task);
	struct mlx4_en_dev *mdev = priv->mdev;
	int err = 0;

	mutex_lock(&mdev->state_lock);
	if (priv->port_up) {
		/* Remove old MAC and insert the new one */
		err = mlx4_replace_mac(mdev->dev, priv->port,
				       priv->base_qpn, priv->mac);
		if (err)
			en_err(priv, "Failed changing HW MAC address\n");
	} else
		en_dbg(HW, priv, "Port is down while "
				 "registering mac, exiting...\n");

	mutex_unlock(&mdev->state_lock);
}

static void mlx4_en_clear_list(struct net_device *dev)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	struct dev_mc_list *plist = priv->mc_list;
	struct dev_mc_list *next;

	while (plist) {
		next = plist->next;
		kfree(plist);
		plist = next;
	}
	priv->mc_list = NULL;
}

static void mlx4_en_cache_mclist(struct net_device *dev)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	struct dev_mc_list *mclist;
	struct dev_mc_list *tmp;
	struct dev_mc_list *plist = NULL;

	mlx4_en_clear_list(dev);
	for (mclist = dev->mc_list; mclist; mclist = mclist->next) {
		tmp = kmalloc(sizeof(struct dev_mc_list), GFP_ATOMIC);
		if (!tmp) {
			en_err(priv, "failed to allocate multicast list\n");
			mlx4_en_clear_list(dev);
			return;
		}
		memcpy(tmp, mclist, sizeof(struct dev_mc_list));
		tmp->next = NULL;
		if (plist)
			plist->next = tmp;
		else
			priv->mc_list = tmp;
		plist = tmp;
	}
}


static void mlx4_en_set_multicast(struct net_device *dev)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);

	if (!priv->port_up)
		return;

	queue_work(priv->mdev->workqueue, &priv->mcast_task);
}

static void mlx4_en_do_set_multicast(struct work_struct *work)
{
	struct mlx4_en_priv *priv = container_of(work, struct mlx4_en_priv,
						 mcast_task);
	struct mlx4_en_dev *mdev = priv->mdev;
	struct net_device *dev = priv->dev;
	struct dev_mc_list *mclist;
	u64 mcast_addr = 0;
	u8 mc_list[16] = {0};
	int err;

	mutex_lock(&mdev->state_lock);
	if (!mdev->device_up) {
		en_dbg(HW, priv, "Card is not up, "
				 "ignoring multicast change.\n");
		goto out;
	}
	if (!priv->port_up) {
		en_dbg(HW, priv, "Port is down, "
				 "ignoring  multicast change.\n");
		goto out;
	}

	if (!netif_carrier_ok(dev)) {
		if (!mlx4_en_QUERY_PORT(mdev, priv->port)) {
			if (priv->port_state.link_state) {
				priv->last_link_state = MLX4_DEV_EVENT_PORT_UP;
				netif_carrier_on(dev);
				en_dbg(LINK, priv, "Link Up\n");
			}
		}
	}

	/*
	 * Promsicuous mode: disable all filters
	 */

	if (dev->flags & IFF_PROMISC) {
		if (!(priv->flags & MLX4_EN_FLAG_PROMISC)) {
			if (netif_msg_rx_status(priv))
				en_warn(priv, "Entering promiscuous mode\n");
			priv->flags |= MLX4_EN_FLAG_PROMISC;

			/* Enable promiscouos mode */
			err = mlx4_SET_PORT_qpn_calc(mdev->dev, priv->port,
						     priv->base_qpn, 1);
			if (err)
				en_err(priv, "Failed enabling "
					     "promiscous mode\n");

			/* Disable port multicast filter (unconditionally) */
			err = mlx4_SET_MCAST_FLTR(mdev->dev, priv->port, 0,
						  0, MLX4_MCAST_DISABLE);
			if (err)
				en_err(priv, "Failed disabling "
					     "multicast filter\n");

			/* Disable port VLAN filter */
			err = mlx4_SET_VLAN_FLTR(mdev->dev, priv->port, NULL);
			if (err)
				en_err(priv, "Failed disabling VLAN filter\n");
		}
		goto out;
	}

	/*
	 * Not in promiscous mode
	 */

	if (priv->flags & MLX4_EN_FLAG_PROMISC) {
		if (netif_msg_rx_status(priv))
			en_warn(priv, "Port:%d leaving promiscuous mode\n",
				  priv->port);
		priv->flags &= ~MLX4_EN_FLAG_PROMISC;

		/* Disable promiscouos mode */
		err = mlx4_SET_PORT_qpn_calc(mdev->dev, priv->port,
					     priv->base_qpn, 0);
		if (err)
			en_err(priv, "Failed disabling promiscous mode\n");

		/* Enable port VLAN filter */
		err = mlx4_SET_VLAN_FLTR(mdev->dev, priv->port, priv->vlgrp);
		if (err)
			en_err(priv, "Failed enabling VLAN filter\n");
	}

	/* Enable/disable the multicast filter according to IFF_ALLMULTI */
	if (dev->flags & IFF_ALLMULTI) {
		err = mlx4_SET_MCAST_FLTR(mdev->dev, priv->port, 0,
					  0, MLX4_MCAST_DISABLE);
		if (err)
			en_err(priv, "Failed disabling multicast filter\n");
	} else {
		err = mlx4_SET_MCAST_FLTR(mdev->dev, priv->port, 0,
					  0, MLX4_MCAST_DISABLE);
		if (err)
			en_err(priv, "Failed disabling multicast filter\n");

		/* Detach our qp from all the multicast addresses */
		for (mclist = priv->mc_list; mclist; mclist = mclist->next) {
			memcpy(&mc_list[10], mclist->dmi_addr, ETH_ALEN);
			mlx4_multicast_detach(mdev->dev, &priv->rss_map.indir_qp,
					      mc_list, MLX4_PROT_ETH);
		}
		/* Flush mcast filter and init it with broadcast address */
		mlx4_SET_MCAST_FLTR(mdev->dev, priv->port, ETH_BCAST,
				    1, MLX4_MCAST_CONFIG);

		/* Update multicast list - we cache all addresses so they won't
		 * change while HW is updated holding the command semaphor */
		netif_tx_lock_bh(dev);
		mlx4_en_cache_mclist(dev);
		netif_tx_unlock_bh(dev);
		for (mclist = priv->mc_list; mclist; mclist = mclist->next) {
			memcpy(&mc_list[10], mclist->dmi_addr, ETH_ALEN);
			mlx4_multicast_attach(mdev->dev, &priv->rss_map.indir_qp,
					      mc_list, 0, MLX4_PROT_ETH);
			mcast_addr = mlx4_en_mac_to_u64(mclist->dmi_addr);
			mlx4_SET_MCAST_FLTR(mdev->dev, priv->port,
					    mcast_addr, 0, MLX4_MCAST_CONFIG);
		}
		err = mlx4_SET_MCAST_FLTR(mdev->dev, priv->port, 0,
					  0, MLX4_MCAST_ENABLE);
		if (err)
			en_err(priv, "Failed enabling multicast filter\n");
	}
out:
	mutex_unlock(&mdev->state_lock);
}

void mlx4_en_netpoll(struct net_device *dev)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	struct mlx4_en_cq *cq;
	unsigned long flags;
	int i;


	for (i = 0; i < priv->rx_ring_num; i++) {
		cq = &priv->rx_cq[i];
		spin_lock_irqsave(&cq->lock, flags);
		while (test_bit(__LINK_STATE_RX_SCHED, &cq->poll_dev->state))
			msleep(1);
		if (priv->rx_ring[i].use_frags)
			mlx4_en_process_rx_cq(dev, cq, 0);
		else
			mlx4_en_process_rx_cq_skb(dev, cq, 0);
		spin_unlock_irqrestore(&cq->lock, flags);
	}
}


static void mlx4_en_tx_timeout(struct net_device *dev)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	struct mlx4_en_dev *mdev = priv->mdev;

	if (netif_msg_timer(priv))
		en_warn(priv, "Tx timeout called\n");

	priv->port_stats.tx_timeout++;
	en_dbg(DRV, priv, "Scheduling watchdog\n");
	queue_work(mdev->workqueue, &priv->watchdog_task);
}


static struct net_device_stats *mlx4_en_get_stats(struct net_device *dev)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);

	spin_lock_bh(&priv->stats_lock);
	memcpy(&priv->ret_stats, &priv->stats, sizeof(priv->stats));
	spin_unlock_bh(&priv->stats_lock);

	return &priv->ret_stats;
}

void mlx4_en_set_default_moderation(struct mlx4_en_priv *priv)
{
	struct mlx4_en_cq *cq;
	int i;

	/* If we haven't received a specific coalescing setting
	 * (module param), we set the moderation paramters as follows:
	 * - moder_cnt is set to the number of mtu sized packets to
	 *   satisfy our coelsing target.
	 * - moder_time is set to a fixed value.
	 */
	priv->rx_frames = MLX4_EN_RX_COAL_TARGET / priv->dev->mtu + 1;
	priv->rx_usecs = MLX4_EN_RX_COAL_TIME;
	en_dbg(INTR, priv, "Default coalesing params for mtu:%d - "
			   "rx_frames:%d rx_usecs:%d\n",
		 priv->dev->mtu, priv->rx_frames, priv->rx_usecs);

	/* Setup cq moderation params */
	for (i = 0; i < priv->rx_ring_num; i++) {
		cq = &priv->rx_cq[i];
		cq->moder_cnt = priv->rx_frames;
		cq->moder_time = priv->rx_usecs;
	}

	for (i = 0; i < priv->tx_ring_num; i++) {
		cq = &priv->tx_cq[i];
		cq->moder_cnt = MLX4_EN_TX_COAL_PKTS;
		cq->moder_time = MLX4_EN_TX_COAL_TIME;
	}

	/* Reset auto-moderation params */
	priv->pkt_rate_low = MLX4_EN_RX_RATE_LOW;
	priv->rx_usecs_low = MLX4_EN_RX_COAL_TIME_LOW;
	priv->pkt_rate_high = MLX4_EN_RX_RATE_HIGH;
	priv->rx_usecs_high = MLX4_EN_RX_COAL_TIME_HIGH;
	priv->sample_interval = MLX4_EN_SAMPLE_INTERVAL;
	priv->adaptive_rx_coal = 1;
	priv->last_moder_time = MLX4_EN_AUTO_CONF;
	priv->last_moder_jiffies = 0;
	priv->last_moder_packets = 0;
	priv->last_moder_tx_packets = 0;
	priv->last_moder_bytes = 0;
}

static void mlx4_en_auto_moderation(struct mlx4_en_priv *priv)
{
	unsigned long period = (unsigned long) (jiffies - priv->last_moder_jiffies);
	struct mlx4_en_cq *cq;
	unsigned long packets;
	unsigned long rate;
	unsigned long avg_pkt_size;
	unsigned long rx_packets;
	unsigned long rx_bytes;
	unsigned long rx_byte_diff;
	unsigned long tx_packets;
	unsigned long tx_pkt_diff;
	unsigned long rx_pkt_diff;
	int moder_time;
	int i, err;

	if (!priv->adaptive_rx_coal || period < priv->sample_interval * HZ)
		return;

	spin_lock_bh(&priv->stats_lock);
	rx_packets = priv->stats.rx_packets;
	rx_bytes = priv->stats.rx_bytes;
	tx_packets = priv->stats.tx_packets;
	spin_unlock_bh(&priv->stats_lock);

	if (!priv->last_moder_jiffies || !period)
		goto out;

	tx_pkt_diff = ((unsigned long) (tx_packets -
					priv->last_moder_tx_packets));
	rx_pkt_diff = ((unsigned long) (rx_packets -
					priv->last_moder_packets));
	packets = max(tx_pkt_diff, rx_pkt_diff);
	rx_byte_diff = rx_bytes - priv->last_moder_bytes;
	rx_byte_diff = rx_byte_diff ? rx_byte_diff : 1;
	rate = packets * HZ / period;
	avg_pkt_size = packets ? ((unsigned long) (rx_bytes -
				 priv->last_moder_bytes)) / packets : 0;

	/* Apply auto-moderation only when packet rate exceeds a rate that
	 * it matters */
	if (rate > MLX4_EN_RX_RATE_THRESH) {
		/* If tx and rx packet rates are not balanced, assume that
		 * traffic is mainly BW bound and apply maximum moderation.
		 * Otherwise, moderate according to packet rate */
		if (2 * tx_pkt_diff > 3 * rx_pkt_diff &&
		    rx_pkt_diff / rx_byte_diff <
		    MLX4_EN_SMALL_PKT_SIZE)
			moder_time = priv->rx_usecs_low;
		else if (2 * rx_pkt_diff > 3 * tx_pkt_diff)
			moder_time = priv->rx_usecs_high;
		else {
			if (rate < priv->pkt_rate_low ||
			    avg_pkt_size < MLX4_EN_AVG_PKT_SMALL)
				moder_time = priv->rx_usecs_low;
			else if (rate > priv->pkt_rate_high)
				moder_time = priv->rx_usecs_high;
			else
				moder_time = (rate - priv->pkt_rate_low) *
					(priv->rx_usecs_high - priv->rx_usecs_low) /
					(priv->pkt_rate_high - priv->pkt_rate_low) +
					priv->rx_usecs_low;
		}
	} else {
		/* When packet rate is low, use default moderation rather than
		 * 0 to prevent interrupt storms if traffic suddenly increases */
		moder_time = priv->rx_usecs;
	}

	en_dbg(INTR, priv, "tx rate:%lu rx_rate:%lu\n",
	       tx_pkt_diff * HZ / period, rx_pkt_diff * HZ / period);

	en_dbg(INTR, priv, "Rx moder_time changed from:%d to %d period:%lu "
	       "[jiff] packets:%lu avg_pkt_size:%lu rate:%lu [p/s])\n",
		 priv->last_moder_time, moder_time, period, packets,
		 avg_pkt_size, rate);

	if (moder_time != priv->last_moder_time) {
		priv->last_moder_time = moder_time;
		for (i = 0; i < priv->rx_ring_num; i++) {
			cq = &priv->rx_cq[i];
			cq->moder_time = moder_time;
			err = mlx4_en_set_cq_moder(priv, cq);
			if (err) {
				en_err(priv, "Failed modifying moderation for cq:%d\n", i);
				break;
			}
		}
	}

out:
	priv->last_moder_packets = rx_packets;
	priv->last_moder_tx_packets = tx_packets;
	priv->last_moder_bytes = rx_bytes;
	priv->last_moder_jiffies = jiffies;
}

static void mlx4_en_do_get_stats(struct work_struct *work)
{
	struct delayed_work *delay = container_of(work, struct delayed_work, work);
	struct mlx4_en_priv *priv = container_of(delay, struct mlx4_en_priv,
						 stats_task);
	struct mlx4_en_dev *mdev = priv->mdev;
	int err;

	err = mlx4_en_DUMP_ETH_STATS(mdev, priv->port, 0);
	if (err)
		en_dbg(HW, priv, "Could not update stats \n");

	mutex_lock(&mdev->state_lock);
	if (mdev->device_up) {
		if (priv->port_up)
			mlx4_en_auto_moderation(priv);

		queue_delayed_work(mdev->workqueue, &priv->stats_task, STATS_DELAY);
	}
	mutex_unlock(&mdev->state_lock);
}

static void mlx4_en_linkstate(struct work_struct *work)
{
	struct mlx4_en_priv *priv = container_of(work, struct mlx4_en_priv,
						 linkstate_task);
	struct mlx4_en_dev *mdev = priv->mdev;
	int linkstate = priv->link_state;

	mutex_lock(&mdev->state_lock);
	/* If observable port state changed set carrier state and
	 * report to system log */
	if (priv->last_link_state != linkstate) {
		if (linkstate == MLX4_DEV_EVENT_PORT_DOWN) {
			en_info(priv, "Link Down\n");
			netif_carrier_off(priv->dev);
		} else {
			en_info(priv, "Link Up\n");
			netif_carrier_on(priv->dev);
		}
	}
	priv->last_link_state = linkstate;
	mutex_unlock(&mdev->state_lock);
}


int mlx4_en_start_port(struct net_device *dev)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	struct mlx4_en_dev *mdev = priv->mdev;
	struct mlx4_en_cq *cq;
	struct mlx4_en_tx_ring *tx_ring;
	int rx_index = 0;
	int tx_index = 0;
	int err = 0;
	int i;
	int j;
	u8 mc_list[16] = {0};

	if (priv->port_up) {
		en_dbg(DRV, priv, "start port called while port already up\n");
		return 0;
	}

	/* Calculate Rx buf size */
	dev->mtu = min(dev->mtu, priv->max_mtu);
	mlx4_en_calc_rx_buf(dev);
	en_dbg(DRV, priv, "Rx buf size:%d\n", priv->rx_skb_size);

	/* Configure rx cq's and rings */
	err = mlx4_en_activate_rx_rings(priv);
	if (err) {
		en_err(priv, "Failed to activate RX rings\n");
		return err;
	}
	for (i = 0; i < priv->rx_ring_num; i++) {
		cq = &priv->rx_cq[i];

		err = mlx4_en_activate_cq(priv, cq);
		if (err) {
			en_err(priv, "Failed activating Rx CQ\n");
			goto cq_err;
		}
		for (j = 0; j < cq->size; j++)
			cq->buf[j].owner_sr_opcode = MLX4_CQE_OWNER_MASK;
		err = mlx4_en_set_cq_moder(priv, cq);
		if (err) {
			en_err(priv, "Failed setting cq moderation parameters");
			mlx4_en_deactivate_cq(priv, cq);
			goto cq_err;
		}
		mlx4_en_arm_cq(priv, cq);
		priv->rx_ring[i].cqn = cq->mcq.cqn;
		++rx_index;
	}

	/* Set port mac number */
	en_dbg(DRV, priv, "Setting mac for port %d\n", priv->port);
	err = mlx4_register_mac(mdev->dev, priv->port,
				priv->mac, &priv->base_qpn);
	if (err) {
		en_err(priv, "Failed setting port mac\n");
		goto cq_err;
	}

	err = mlx4_en_config_rss_steer(priv);
	if (err) {
		en_err(priv, "Failed configuring rss steering\n");
		goto mac_err;
	}

	/* Configure tx cq's and rings */
	for (i = 0; i < priv->tx_ring_num; i++) {
		/* Configure cq */
		cq = &priv->tx_cq[i];
		err = mlx4_en_activate_cq(priv, cq);
		if (err) {
			en_err(priv, "Failed allocating Tx CQ\n");
			goto tx_err;
		}
		err = mlx4_en_set_cq_moder(priv, cq);
		if (err) {
			en_err(priv, "Failed setting cq moderation parameters");
			mlx4_en_deactivate_cq(priv, cq);
			goto tx_err;
		}
		en_dbg(DRV, priv, "Resetting index of collapsed CQ:%d "
				  "to -1\n", i);
		cq->buf->wqe_index = 0xffff;

		/* Configure ring */
		tx_ring = &priv->tx_ring[i];
		err = mlx4_en_activate_tx_ring(priv, tx_ring, cq->mcq.cqn);
		if (err) {
			en_err(priv, "Failed allocating Tx ring\n");
			mlx4_en_deactivate_cq(priv, cq);
			goto tx_err;
		}
		/* Set initial ownership of all Tx TXBBs to SW (1) */
		for (j = 0; j < tx_ring->buf_size; j += STAMP_STRIDE)
			*((u32 *) (tx_ring->buf + j)) = 0xffffffff;
		++tx_index;
	}

	for (i = 0; i < MLX4_EN_TX_HASH_SIZE; i++) {
		memset(&priv->tx_hash[i], 0, sizeof(struct mlx4_en_tx_hash_entry));
		/*
		 * Initially, all streams are assigned to the rings
		 * that should handle the small packages streams, (the lower ring
		 * indixes) then moved according the stream charasteristics.
		 */
		priv->tx_hash[i].ring = i & (MLX4_EN_NUM_HASH_RINGS / 2 - 1);
	}

	/* Configure port */
	err = mlx4_SET_PORT_general(mdev->dev, priv->port,
				    priv->rx_skb_size + ETH_FCS_LEN,
				    priv->prof->tx_pause,
				    priv->prof->tx_ppp,
				    priv->prof->rx_pause,
				    priv->prof->rx_ppp);
	if (err) {
		en_err(priv, "Failed setting port general configurations"
			       " for port %d, with error %d\n", priv->port, err);
		goto tx_err;
	}
	/* Set default qp number */
	err = mlx4_SET_PORT_qpn_calc(mdev->dev, priv->port, priv->base_qpn, 0);
	if (err) {
		en_err(priv, "Failed setting default qp numbers\n");
		goto tx_err;
	}

	/* Init port */
	en_dbg(HW, priv, "Initializing port\n");
	err = mlx4_INIT_PORT(mdev->dev, priv->port);
	if (err) {
		en_err(priv, "Failed Initializing port\n");
		goto tx_err;
	}

	/* Attach rx QP to bradcast address */
	memset(&mc_list[10], 0xff, ETH_ALEN);
	if (mlx4_multicast_attach(mdev->dev, &priv->rss_map.indir_qp, mc_list,
				  0, MLX4_PROT_ETH))
		mlx4_warn(mdev, "Failed Attaching Broadcast\n");

	/* Schedule multicast task to populate multicast list */
	queue_work(mdev->workqueue, &priv->mcast_task);

	priv->port_up = true;
	netif_start_queue(dev);
	return 0;

tx_err:
	while (tx_index--) {
		mlx4_en_deactivate_tx_ring(priv, &priv->tx_ring[tx_index]);
		mlx4_en_deactivate_cq(priv, &priv->tx_cq[tx_index]);
	}

	mlx4_en_release_rss_steer(priv);
mac_err:
	mlx4_unregister_mac(mdev->dev, priv->port, priv->base_qpn);
cq_err:
	while (rx_index--)
		mlx4_en_deactivate_cq(priv, &priv->rx_cq[rx_index]);
	for (i = 0; i < priv->rx_ring_num; i++)
		mlx4_en_deactivate_rx_ring(priv, &priv->rx_ring[i]);

	return err; /* need to close devices */
}


void mlx4_en_stop_port(struct net_device *dev)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	struct mlx4_en_dev *mdev = priv->mdev;
	struct dev_mc_list *mclist;
	int i;
	u8 mc_list[16] = {0};

	if (!priv->port_up) {
		en_dbg(DRV, priv, "stop port called while port already down\n");
		return;
	}
	netif_stop_queue(dev);

	/* Synchronize with tx routine */
	netif_tx_lock_bh(dev);
	netif_stop_queue(dev);
	netif_tx_unlock_bh(dev);

	/* Set port as not active */
	priv->port_up = false;

	/* Detach All multicasts */
	memset(&mc_list[10], 0xff, ETH_ALEN);
	mlx4_multicast_detach(mdev->dev, &priv->rss_map.indir_qp, mc_list,
			      MLX4_PROT_ETH);
	for (mclist = priv->mc_list; mclist; mclist = mclist->next) {
		memcpy(&mc_list[10], mclist->dmi_addr, ETH_ALEN);
		mlx4_multicast_detach(mdev->dev, &priv->rss_map.indir_qp,
				      mc_list, MLX4_PROT_ETH);
	}
	mlx4_en_clear_list(dev);

	/* Unregister Mac address for the port */
	mlx4_unregister_mac(mdev->dev, priv->port, priv->base_qpn);

	/* Free TX Rings */
	for (i = 0; i < priv->tx_ring_num; i++) {
		mlx4_en_deactivate_tx_ring(priv, &priv->tx_ring[i]);
		mlx4_en_deactivate_cq(priv, &priv->tx_cq[i]);
	}
	msleep(10);

	for (i = 0; i < priv->tx_ring_num; i++)
		mlx4_en_free_tx_buf(dev, &priv->tx_ring[i]);

	/* Free RSS qps */
	mlx4_en_release_rss_steer(priv);

	/* Free RX Rings */
	for (i = 0; i < priv->rx_ring_num; i++) {
		mlx4_en_deactivate_rx_ring(priv, &priv->rx_ring[i]);
		mlx4_en_deactivate_cq(priv, &priv->rx_cq[i]);
	}

	/* close port*/
	mlx4_CLOSE_PORT(mdev->dev, priv->port);
}

static void mlx4_en_restart(struct work_struct *work)
{
	struct mlx4_en_priv *priv = container_of(work, struct mlx4_en_priv,
						 watchdog_task);
	struct mlx4_en_dev *mdev = priv->mdev;
	struct net_device *dev = priv->dev;

	en_dbg(DRV, priv, "Watchdog task called \n");

	mutex_lock(&mdev->state_lock);
	if (priv->port_up) {
		mlx4_en_stop_port(dev);
		if (mlx4_en_start_port(dev))
		    en_err(priv, "Failed restarting port\n");
	}
	mutex_unlock(&mdev->state_lock);
}


int mlx4_en_open(struct net_device *dev)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	struct mlx4_en_dev *mdev = priv->mdev;
	int i;
	int err = 0;

	mutex_lock(&mdev->state_lock);

	if (!mdev->device_up) {
		en_err(priv, "Cannot open - device down/disabled\n");
		err = -EBUSY;
		goto out;
	}

	/* Reset HW statistics and performance counters */
	if (mlx4_en_DUMP_ETH_STATS(mdev, priv->port, 1))
		en_dbg(HW, priv, "Failed dumping statistics\n");

	memset(&priv->stats, 0, sizeof(priv->stats));
	memset(&priv->pstats, 0, sizeof(priv->pstats));

	for (i = 0; i < priv->tx_ring_num; i++) {
		priv->tx_ring[i].bytes = 0;
		priv->tx_ring[i].packets = 0;
	}
	for (i = 0; i < priv->rx_ring_num; i++) {
		priv->rx_ring[i].bytes = 0;
		priv->rx_ring[i].packets = 0;
	}

	mlx4_en_set_default_moderation(priv);
	err = mlx4_en_start_port(dev);
	if (err)
		en_err(priv, "Failed starting port:%d\n", priv->port);

out:
	mutex_unlock(&mdev->state_lock);
	return err;
}


int mlx4_en_close(struct net_device *dev)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	struct mlx4_en_dev *mdev = priv->mdev;

	en_dbg(IFDOWN, priv, "Close port called\n");

	mutex_lock(&mdev->state_lock);

	mlx4_en_stop_port(dev);
	netif_carrier_off(dev);

	mutex_unlock(&mdev->state_lock);
	return 0;
}

void mlx4_en_free_resources(struct mlx4_en_priv *priv)
{
	int i;

	for (i = 0; i < priv->tx_ring_num; i++) {
		if (priv->tx_ring[i].tx_info)
			mlx4_en_destroy_tx_ring(priv, &priv->tx_ring[i]);
		if (priv->tx_cq[i].buf)
			mlx4_en_destroy_cq(priv, &priv->tx_cq[i]);
	}

	for (i = 0; i < priv->rx_ring_num; i++) {
		if (priv->rx_ring[i].rx_info)
			mlx4_en_destroy_rx_ring(priv, &priv->rx_ring[i]);
		if (priv->rx_cq[i].buf)
			mlx4_en_destroy_cq(priv, &priv->rx_cq[i]);
	}
}

int mlx4_en_alloc_resources(struct mlx4_en_priv *priv)
{
	struct mlx4_en_port_profile *prof = priv->prof;
	int i;

	/* Create tx Rings */
	for (i = 0; i < priv->tx_ring_num; i++) {
		if (mlx4_en_create_cq(priv, &priv->tx_cq[i],
				      prof->tx_ring_size, i, TX))
			goto err;

		if (mlx4_en_create_tx_ring(priv, &priv->tx_ring[i],
					   prof->tx_ring_size, TXBB_SIZE))
			goto err;
	}

	/* Create rx Rings */
	for (i = 0; i < priv->rx_ring_num; i++) {
		if (i > 0)
			priv->rx_ring[i].use_frags = 1;
		else
			priv->rx_ring[i].use_frags = 0;

		if (mlx4_en_create_cq(priv, &priv->rx_cq[i],
				      prof->rx_ring_size, i, RX))
			goto err;

		if (mlx4_en_create_rx_ring(priv, &priv->rx_ring[i],
					   prof->rx_ring_size))
			goto err;
	}

	return 0;

err:
	en_err(priv, "Failed to allocate NIC resources\n");
	return -ENOMEM;
}


void mlx4_en_destroy_netdev(struct net_device *dev)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	struct mlx4_en_dev *mdev = priv->mdev;

	en_dbg(DRV, priv, "Destroying netdev on port:%d\n", priv->port);

	/* Unregsiter device - this will close the port if it was up */
	if (priv->registered)
		unregister_netdev(dev);

	if (priv->allocated)
		mlx4_free_hwq_res(mdev->dev, &priv->res, MLX4_EN_PAGE_SIZE);

	cancel_delayed_work(&priv->stats_task);
	/* flush any pending task for this netdev */
	flush_workqueue(mdev->workqueue);

	/* Detach the netdev so tasks would not attempt to access it */
	mutex_lock(&mdev->state_lock);
	mdev->pndev[priv->port] = NULL;
	mutex_unlock(&mdev->state_lock);

	mlx4_en_free_resources(priv);
	free_netdev(dev);
}

static int mlx4_en_change_mtu(struct net_device *dev, int new_mtu)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	struct mlx4_en_dev *mdev = priv->mdev;
	int err = 0;

	en_dbg(DRV, priv, "Change MTU called - current:%d new:%d\n",
		 dev->mtu, new_mtu);

	if ((new_mtu < MLX4_EN_MIN_MTU) || (new_mtu > priv->max_mtu)) {
		en_err(priv, "Bad MTU size:%d.\n", new_mtu);
		return -EPERM;
	}
	dev->mtu = new_mtu;

	if (netif_running(dev)) {
		mutex_lock(&mdev->state_lock);
		if (!mdev->device_up) {
			/* NIC is probably restarting - let watchdog task reset
			 * the port */
			en_dbg(DRV, priv, "Change MTU called with card down!?\n");
		} else {
			mlx4_en_stop_port(dev);
			mlx4_en_set_default_moderation(priv);
			err = mlx4_en_start_port(dev);
			if (err) {
				en_err(priv, "Failed restarting port:%d\n",
					 priv->port);
				queue_work(mdev->workqueue, &priv->watchdog_task);
			}
		}
		mutex_unlock(&mdev->state_lock);
	}
	return 0;
}

int mlx4_en_init_netdev(struct mlx4_en_dev *mdev, int port,
			struct mlx4_en_port_profile *prof)
{
	struct net_device *dev;
	struct mlx4_en_priv *priv;
	int i;
	int err;

	dev = alloc_etherdev(sizeof(struct mlx4_en_priv));
	if (dev == NULL) {
		mlx4_err(mdev, "Net device allocation failed\n");
		return -ENOMEM;
	}

	SET_NETDEV_DEV(dev, &mdev->dev->pdev->dev);

	/*
	 * Initialize driver private data
	 */

	priv = netdev_priv(dev);
	memset(priv, 0, sizeof(struct mlx4_en_priv));
	priv->dev = dev;
	priv->mdev = mdev;
	priv->prof = prof;
	priv->port = port;
	priv->port_up = false;
	priv->rx_csum = 1;
	priv->flags = prof->flags;
	priv->tx_ring_num = prof->tx_ring_num;
	priv->rx_ring_num = prof->rx_ring_num;
	priv->mc_list = NULL;
	priv->mac_index = -1;
	priv->msg_enable = MLX4_EN_MSG_LEVEL;
	spin_lock_init(&priv->stats_lock);
	INIT_WORK(&priv->mcast_task, mlx4_en_do_set_multicast);
	INIT_WORK(&priv->mac_task, mlx4_en_do_set_mac);
	INIT_WORK(&priv->watchdog_task, mlx4_en_restart);
	INIT_WORK(&priv->linkstate_task, mlx4_en_linkstate);
	INIT_DELAYED_WORK(&priv->stats_task, mlx4_en_do_get_stats);

	/* Query for default mac and max mtu */
	priv->max_mtu = mdev->dev->caps.eth_mtu_cap[priv->port];
	priv->mac = mdev->dev->caps.def_mac[priv->port];
	if (ILLEGAL_MAC(priv->mac)) {
		en_err(priv, "Port: %d, invalid mac burned: 0x%llx, quiting\n",
			 priv->port, priv->mac);
		err = -EINVAL;
		goto out;
	}

	err = mlx4_en_alloc_resources(priv);
	if (err)
		goto out;

	/* Allocate page for receive rings */
	err = mlx4_alloc_hwq_res(mdev->dev, &priv->res,
				MLX4_EN_PAGE_SIZE, MLX4_EN_PAGE_SIZE);
	if (err) {
		en_err(priv, "Failed to allocate page for rx qps\n");
		goto out;
	}
	priv->allocated = true;

	/* Populate Tx priority mappings */
	mlx4_en_set_prio_map(priv, priv->tx_prio_map,
			     prof->tx_ring_num - MLX4_EN_NUM_HASH_RINGS);

	/*
	 * Initialize netdev entry points
	 */

	dev->open = &mlx4_en_open;
	dev->stop = &mlx4_en_close;
	dev->hard_start_xmit = &mlx4_en_xmit;
	dev->get_stats = &mlx4_en_get_stats;
	dev->set_multicast_list = &mlx4_en_set_multicast;
	dev->set_mac_address = &mlx4_en_set_mac;
	dev->change_mtu = &mlx4_en_change_mtu;
	dev->tx_timeout = &mlx4_en_tx_timeout;
	dev->watchdog_timeo = MLX4_EN_WATCHDOG_TIMEOUT;
	dev->vlan_rx_register = mlx4_en_vlan_rx_register;
	dev->vlan_rx_add_vid = mlx4_en_vlan_rx_add_vid;
	dev->vlan_rx_kill_vid = mlx4_en_vlan_rx_kill_vid;
#ifdef CONFIG_NET_POLL_CONTROLLER
	dev->poll_controller = mlx4_en_netpoll;
#endif
	SET_ETHTOOL_OPS(dev, &mlx4_en_ethtool_ops);

	/* Set defualt MAC */
	dev->addr_len = ETH_ALEN;
	for (i = 0; i < ETH_ALEN; i++) {
		dev->dev_addr[ETH_ALEN - 1 - i] = (u8) (priv->mac >> (8 * i));
		dev->perm_addr[ETH_ALEN - 1 - i] = (u8) (priv->mac >> (8 * i));
	}

	/*
	 * Set driver features
	 */
	dev->features |= NETIF_F_SG;
	dev->features |= NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM;
#ifdef HAVE_NETDEV_VLAN_FEATURES
	dev->vlan_features |= NETIF_F_SG;
	dev->vlan_features |= NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM;
#else
	dev->features |= NETIF_F_VLAN_CSUM;
#endif
	dev->features |= NETIF_F_HIGHDMA;
	dev->features |= NETIF_F_HW_VLAN_TX |
			 NETIF_F_HW_VLAN_RX |
			 NETIF_F_HW_VLAN_FILTER;
	if (mdev->profile.num_lro)
		dev->features |= NETIF_F_LRO;
	if (mdev->LSO_support) {
		dev->features |= NETIF_F_TSO;
		dev->features |= NETIF_F_TSO6;
#ifdef HAVE_NETDEV_VLAN_FEATURES
		dev->vlan_features |= NETIF_F_TSO;
		dev->vlan_features |= NETIF_F_TSO6;
#else
		dev->features |= NETIF_F_VLAN_TSO;
#endif
	}


	mdev->pndev[port] = dev;

	netif_carrier_off(dev);
	err = register_netdev(dev);
	if (err) {
		mlx4_err(mdev, "Netdev registration failed for port %d\n", port);
		goto out;
	}

	en_warn(priv, "Using %d TX rings\n", prof->tx_ring_num);
	en_warn(priv, "Using %d RX rings\n", prof->rx_ring_num);

	priv->registered = true;
	queue_delayed_work(mdev->workqueue, &priv->stats_task, STATS_DELAY);
	return 0;

out:
	mlx4_en_destroy_netdev(dev);
	return err;
}

