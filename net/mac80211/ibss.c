/*
 * IBSS mode implementation
 * Copyright 2003-2008, Jouni Malinen <j@w1.fi>
 * Copyright 2004, Instant802 Networks, Inc.
 * Copyright 2005, Devicescape Software, Inc.
 * Copyright 2006-2007	Jiri Benc <jbenc@suse.cz>
 * Copyright 2007, Michael Wu <flamingice@sourmilk.net>
 * Copyright 2009, Johannes Berg <johannes@sipsolutions.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/if_ether.h>
#include <linux/skbuff.h>
#include <linux/if_arp.h>
#include <linux/etherdevice.h>
#include <linux/rtnetlink.h>
#include <net/mac80211.h>
#include <asm/unaligned.h>

#include "ieee80211_i.h"
#include "driver-ops.h"
#include "rate.h"

#define IEEE80211_SCAN_INTERVAL (2 * HZ)
#define IEEE80211_SCAN_INTERVAL_SLOW (15 * HZ)
#define IEEE80211_IBSS_JOIN_TIMEOUT (7 * HZ)

#define IEEE80211_IBSS_MERGE_INTERVAL (30 * HZ)
#define IEEE80211_IBSS_MERGE_DELAY 0x400000
#define IEEE80211_IBSS_INACTIVITY_LIMIT (60 * HZ)

#define IEEE80211_IBSS_MAX_STA_ENTRIES 128


static void ieee80211_rx_mgmt_auth_ibss(struct ieee80211_sub_if_data *sdata,
					struct ieee80211_mgmt *mgmt,
					size_t len)
{
	u16 auth_alg, auth_transaction, status_code;

	if (len < 24 + 6)
		return;

	auth_alg = le16_to_cpu(mgmt->u.auth.auth_alg);
	auth_transaction = le16_to_cpu(mgmt->u.auth.auth_transaction);
	status_code = le16_to_cpu(mgmt->u.auth.status_code);

	/*
	 * IEEE 802.11 standard does not require authentication in IBSS
	 * networks and most implementations do not seem to use it.
	 * However, try to reply to authentication attempts if someone
	 * has actually implemented this.
	 */
	if (auth_alg == WLAN_AUTH_OPEN && auth_transaction == 1)
		ieee80211_send_auth(sdata, 2, WLAN_AUTH_OPEN, NULL, 0,
				    sdata->u.ibss.bssid, NULL, 0, 0);
}

static void __ieee80211_sta_join_ibss(struct ieee80211_sub_if_data *sdata,
				      const u8 *bssid, const int beacon_int,
				      struct ieee80211_channel *chan,
				      const u32 basic_rates,
				      const u16 capability, u64 tsf)
{
	struct ieee80211_if_ibss *ifibss = &sdata->u.ibss;
	struct ieee80211_local *local = sdata->local;
	int rates, i;
	struct sk_buff *skb;
	struct ieee80211_mgmt *mgmt;
	u8 *pos;
	struct ieee80211_supported_band *sband;
	struct cfg80211_bss *bss;
	u32 bss_change;
	u8 supp_rates[IEEE80211_MAX_SUPP_RATES];

	/* Reset own TSF to allow time synchronization work. */
	drv_reset_tsf(local);

	skb = ifibss->skb;
	rcu_assign_pointer(ifibss->presp, NULL);
	synchronize_rcu();
	skb->data = skb->head;
	skb->len = 0;
	skb_reset_tail_pointer(skb);
	skb_reserve(skb, sdata->local->hw.extra_tx_headroom);

	if (memcmp(ifibss->bssid, bssid, ETH_ALEN))
		sta_info_flush(sdata->local, sdata);

	memcpy(ifibss->bssid, bssid, ETH_ALEN);

	sdata->drop_unencrypted = capability & WLAN_CAPABILITY_PRIVACY ? 1 : 0;

	local->oper_channel = chan;
	local->oper_channel_type = NL80211_CHAN_NO_HT;
	ieee80211_hw_config(local, IEEE80211_CONF_CHANGE_CHANNEL);

	sband = local->hw.wiphy->bands[chan->band];

	/* build supported rates array */
	pos = supp_rates;
	for (i = 0; i < sband->n_bitrates; i++) {
		int rate = sband->bitrates[i].bitrate;
		u8 basic = 0;
		if (basic_rates & BIT(i))
			basic = 0x80;
		*pos++ = basic | (u8) (rate / 5);
	}

	/* Build IBSS probe response */
	mgmt = (void *) skb_put(skb, 24 + sizeof(mgmt->u.beacon));
	memset(mgmt, 0, 24 + sizeof(mgmt->u.beacon));
	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
					  IEEE80211_STYPE_PROBE_RESP);
	memset(mgmt->da, 0xff, ETH_ALEN);
	memcpy(mgmt->sa, sdata->dev->dev_addr, ETH_ALEN);
	memcpy(mgmt->bssid, ifibss->bssid, ETH_ALEN);
	mgmt->u.beacon.beacon_int = cpu_to_le16(beacon_int);
	mgmt->u.beacon.timestamp = cpu_to_le64(tsf);
	mgmt->u.beacon.capab_info = cpu_to_le16(capability);

	pos = skb_put(skb, 2 + ifibss->ssid_len);
	*pos++ = WLAN_EID_SSID;
	*pos++ = ifibss->ssid_len;
	memcpy(pos, ifibss->ssid, ifibss->ssid_len);

	rates = sband->n_bitrates;
	if (rates > 8)
		rates = 8;
	pos = skb_put(skb, 2 + rates);
	*pos++ = WLAN_EID_SUPP_RATES;
	*pos++ = rates;
	memcpy(pos, supp_rates, rates);

	if (sband->band == IEEE80211_BAND_2GHZ) {
		pos = skb_put(skb, 2 + 1);
		*pos++ = WLAN_EID_DS_PARAMS;
		*pos++ = 1;
		*pos++ = ieee80211_frequency_to_channel(chan->center_freq);
	}

	pos = skb_put(skb, 2 + 2);
	*pos++ = WLAN_EID_IBSS_PARAMS;
	*pos++ = 2;
	/* FIX: set ATIM window based on scan results */
	*pos++ = 0;
	*pos++ = 0;

	if (sband->n_bitrates > 8) {
		rates = sband->n_bitrates - 8;
		pos = skb_put(skb, 2 + rates);
		*pos++ = WLAN_EID_EXT_SUPP_RATES;
		*pos++ = rates;
		memcpy(pos, &supp_rates[8], rates);
	}

	if (ifibss->ie_len)
		memcpy(skb_put(skb, ifibss->ie_len),
		       ifibss->ie, ifibss->ie_len);

	rcu_assign_pointer(ifibss->presp, skb);

	sdata->vif.bss_conf.beacon_int = beacon_int;
	bss_change = BSS_CHANGED_BEACON_INT;
	bss_change |= ieee80211_reset_erp_info(sdata);
	bss_change |= BSS_CHANGED_BSSID;
	bss_change |= BSS_CHANGED_BEACON;
	bss_change |= BSS_CHANGED_BEACON_ENABLED;
	ieee80211_bss_info_change_notify(sdata, bss_change);

	ieee80211_sta_def_wmm_params(sdata, sband->n_bitrates, supp_rates);

	ifibss->state = IEEE80211_IBSS_MLME_JOINED;
	mod_timer(&ifibss->timer,
		  round_jiffies(jiffies + IEEE80211_IBSS_MERGE_INTERVAL));

	bss = cfg80211_inform_bss_frame(local->hw.wiphy, local->hw.conf.channel,
					mgmt, skb->len, 0, GFP_KERNEL);
	cfg80211_put_bss(bss);
	cfg80211_ibss_joined(sdata->dev, ifibss->bssid, GFP_KERNEL);
}

static void ieee80211_sta_join_ibss(struct ieee80211_sub_if_data *sdata,
				    struct ieee80211_bss *bss)
{
	struct ieee80211_supported_band *sband;
	u32 basic_rates;
	int i, j;
	u16 beacon_int = bss->cbss.beacon_interval;

	if (beacon_int < 10)
		beacon_int = 10;

	sband = sdata->local->hw.wiphy->bands[bss->cbss.channel->band];

	basic_rates = 0;

	for (i = 0; i < bss->supp_rates_len; i++) {
		int rate = (bss->supp_rates[i] & 0x7f) * 5;
		bool is_basic = !!(bss->supp_rates[i] & 0x80);

		for (j = 0; j < sband->n_bitrates; j++) {
			if (sband->bitrates[j].bitrate == rate) {
				if (is_basic)
					basic_rates |= BIT(j);
				break;
			}
		}
	}

	__ieee80211_sta_join_ibss(sdata, bss->cbss.bssid,
				  beacon_int,
				  bss->cbss.channel,
				  basic_rates,
				  bss->cbss.capability,
				  bss->cbss.tsf);
}

static void ieee80211_rx_bss_info(struct ieee80211_sub_if_data *sdata,
				  struct ieee80211_mgmt *mgmt,
				  size_t len,
				  struct ieee80211_rx_status *rx_status,
				  struct ieee802_11_elems *elems,
				  bool beacon)
{
	struct ieee80211_local *local = sdata->local;
	int freq;
	struct ieee80211_bss *bss;
	struct sta_info *sta;
	struct ieee80211_channel *channel;
	u64 beacon_timestamp, rx_timestamp;
	u32 supp_rates = 0;
	enum ieee80211_band band = rx_status->band;

	if (elems->ds_params && elems->ds_params_len == 1)
		freq = ieee80211_channel_to_frequency(elems->ds_params[0]);
	else
		freq = rx_status->freq;

	channel = ieee80211_get_channel(local->hw.wiphy, freq);

	if (!channel || channel->flags & IEEE80211_CHAN_DISABLED)
		return;

	if (sdata->vif.type == NL80211_IFTYPE_ADHOC && elems->supp_rates &&
	    memcmp(mgmt->bssid, sdata->u.ibss.bssid, ETH_ALEN) == 0) {
		supp_rates = ieee80211_sta_get_rates(local, elems, band);

		rcu_read_lock();

		sta = sta_info_get(local, mgmt->sa);
		if (sta) {
			u32 prev_rates;

			prev_rates = sta->sta.supp_rates[band];
			/* make sure mandatory rates are always added */
			sta->sta.supp_rates[band] = supp_rates |
				ieee80211_mandatory_rates(local, band);

#ifdef CONFIG_MAC80211_IBSS_DEBUG
			if (sta->sta.supp_rates[band] != prev_rates)
				printk(KERN_DEBUG "%s: updated supp_rates set "
				    "for %pM based on beacon info (0x%llx | "
				    "0x%llx -> 0x%llx)\n",
				    sdata->dev->name,
				    sta->sta.addr,
				    (unsigned long long) prev_rates,
				    (unsigned long long) supp_rates,
				    (unsigned long long) sta->sta.supp_rates[band]);
#endif
		} else
			ieee80211_ibss_add_sta(sdata, mgmt->bssid, mgmt->sa, supp_rates);

		rcu_read_unlock();
	}

	bss = ieee80211_bss_info_update(local, rx_status, mgmt, len, elems,
					channel, beacon);
	if (!bss)
		return;

	/* was just updated in ieee80211_bss_info_update */
	beacon_timestamp = bss->cbss.tsf;

	/* check if we need to merge IBSS */

	/* merge only on beacons (???) */
	if (!beacon)
		goto put_bss;

	/* we use a fixed BSSID */
	if (sdata->u.ibss.bssid)
		goto put_bss;

	/* not an IBSS */
	if (!(bss->cbss.capability & WLAN_CAPABILITY_IBSS))
		goto put_bss;

	/* different channel */
	if (bss->cbss.channel != local->oper_channel)
		goto put_bss;

	/* different SSID */
	if (elems->ssid_len != sdata->u.ibss.ssid_len ||
	    memcmp(elems->ssid, sdata->u.ibss.ssid,
				sdata->u.ibss.ssid_len))
		goto put_bss;

	/* same BSSID */
	if (memcmp(bss->cbss.bssid, sdata->u.ibss.bssid, ETH_ALEN) == 0)
		goto put_bss;

	if (rx_status->flag & RX_FLAG_TSFT) {
		/*
		 * For correct IBSS merging we need mactime; since mactime is
		 * defined as the time the first data symbol of the frame hits
		 * the PHY, and the timestamp of the beacon is defined as "the
		 * time that the data symbol containing the first bit of the
		 * timestamp is transmitted to the PHY plus the transmitting
		 * STA's delays through its local PHY from the MAC-PHY
		 * interface to its interface with the WM" (802.11 11.1.2)
		 * - equals the time this bit arrives at the receiver - we have
		 * to take into account the offset between the two.
		 *
		 * E.g. at 1 MBit that means mactime is 192 usec earlier
		 * (=24 bytes * 8 usecs/byte) than the beacon timestamp.
		 */
		int rate;

		if (rx_status->flag & RX_FLAG_HT)
			rate = 65; /* TODO: HT rates */
		else
			rate = local->hw.wiphy->bands[band]->
				bitrates[rx_status->rate_idx].bitrate;

		rx_timestamp = rx_status->mactime + (24 * 8 * 10 / rate);
	} else {
		/*
		 * second best option: get current TSF
		 * (will return -1 if not supported)
		 */
		rx_timestamp = drv_get_tsf(local);
	}

#ifdef CONFIG_MAC80211_IBSS_DEBUG
	printk(KERN_DEBUG "RX beacon SA=%pM BSSID="
	       "%pM TSF=0x%llx BCN=0x%llx diff=%lld @%lu\n",
	       mgmt->sa, mgmt->bssid,
	       (unsigned long long)rx_timestamp,
	       (unsigned long long)beacon_timestamp,
	       (unsigned long long)(rx_timestamp - beacon_timestamp),
	       jiffies);
#endif

	/* give slow hardware some time to do the TSF sync */
	if (rx_timestamp < IEEE80211_IBSS_MERGE_DELAY)
		goto put_bss;

	if (beacon_timestamp > rx_timestamp) {
#ifdef CONFIG_MAC80211_IBSS_DEBUG
		printk(KERN_DEBUG "%s: beacon TSF higher than "
		       "local TSF - IBSS merge with BSSID %pM\n",
		       sdata->dev->name, mgmt->bssid);
#endif
		ieee80211_sta_join_ibss(sdata, bss);
		ieee80211_ibss_add_sta(sdata, mgmt->bssid, mgmt->sa, supp_rates);
	}

 put_bss:
	ieee80211_rx_bss_put(local, bss);
}

/*
 * Add a new IBSS station, will also be called by the RX code when,
 * in IBSS mode, receiving a frame from a yet-unknown station, hence
 * must be callable in atomic context.
 */
struct sta_info *ieee80211_ibss_add_sta(struct ieee80211_sub_if_data *sdata,
					u8 *bssid,u8 *addr, u32 supp_rates)
{
	struct ieee80211_local *local = sdata->local;
	struct sta_info *sta;
	int band = local->hw.conf.channel->band;

	/*
	 * XXX: Consider removing the least recently used entry and
	 * 	allow new one to be added.
	 */
	if (local->num_sta >= IEEE80211_IBSS_MAX_STA_ENTRIES) {
		if (net_ratelimit())
			printk(KERN_DEBUG "%s: No room for a new IBSS STA entry %pM\n",
			       sdata->dev->name, addr);
		return NULL;
	}

	if (compare_ether_addr(bssid, sdata->u.ibss.bssid))
		return NULL;

#ifdef CONFIG_MAC80211_VERBOSE_DEBUG
	printk(KERN_DEBUG "%s: Adding new IBSS station %pM (dev=%s)\n",
	       wiphy_name(local->hw.wiphy), addr, sdata->dev->name);
#endif

	sta = sta_info_alloc(sdata, addr, GFP_ATOMIC);
	if (!sta)
		return NULL;

	set_sta_flags(sta, WLAN_STA_AUTHORIZED);

	/* make sure mandatory rates are always added */
	sta->sta.supp_rates[band] = supp_rates |
			ieee80211_mandatory_rates(local, band);

	rate_control_rate_init(sta);

	if (sta_info_insert(sta))
		return NULL;

	return sta;
}

static int ieee80211_sta_active_ibss(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_local *local = sdata->local;
	int active = 0;
	struct sta_info *sta;

	rcu_read_lock();

	list_for_each_entry_rcu(sta, &local->sta_list, list) {
		if (sta->sdata == sdata &&
		    time_after(sta->last_rx + IEEE80211_IBSS_MERGE_INTERVAL,
			       jiffies)) {
			active++;
			break;
		}
	}

	rcu_read_unlock();

	return active;
}


static void ieee80211_sta_merge_ibss(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_if_ibss *ifibss = &sdata->u.ibss;

	mod_timer(&ifibss->timer,
		  round_jiffies(jiffies + IEEE80211_IBSS_MERGE_INTERVAL));

	ieee80211_sta_expire(sdata, IEEE80211_IBSS_INACTIVITY_LIMIT);

	if (time_before(jiffies, ifibss->last_scan_completed +
		       IEEE80211_IBSS_MERGE_INTERVAL))
		return;

	if (ieee80211_sta_active_ibss(sdata))
		return;

	if (ifibss->fixed_channel)
		return;

	printk(KERN_DEBUG "%s: No active IBSS STAs - trying to scan for other "
	       "IBSS networks with same SSID (merge)\n", sdata->dev->name);

	ieee80211_request_internal_scan(sdata, ifibss->ssid, ifibss->ssid_len);
}

static void ieee80211_sta_create_ibss(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_if_ibss *ifibss = &sdata->u.ibss;
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_supported_band *sband;
	u8 bssid[ETH_ALEN];
	u16 capability;
	int i;

	if (ifibss->fixed_bssid) {
		memcpy(bssid, ifibss->bssid, ETH_ALEN);
	} else {
		/* Generate random, not broadcast, locally administered BSSID. Mix in
		 * own MAC address to make sure that devices that do not have proper
		 * random number generator get different BSSID. */
		get_random_bytes(bssid, ETH_ALEN);
		for (i = 0; i < ETH_ALEN; i++)
			bssid[i] ^= sdata->dev->dev_addr[i];
		bssid[0] &= ~0x01;
		bssid[0] |= 0x02;
	}

	printk(KERN_DEBUG "%s: Creating new IBSS network, BSSID %pM\n",
	       sdata->dev->name, bssid);

	sband = local->hw.wiphy->bands[ifibss->channel->band];

	capability = WLAN_CAPABILITY_IBSS;

	if (ifibss->privacy)
		capability |= WLAN_CAPABILITY_PRIVACY;
	else
		sdata->drop_unencrypted = 0;

	__ieee80211_sta_join_ibss(sdata, bssid, sdata->vif.bss_conf.beacon_int,
				  ifibss->channel, 3, /* first two are basic */
				  capability, 0);
}

static void ieee80211_sta_find_ibss(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_if_ibss *ifibss = &sdata->u.ibss;
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_bss *bss;
	struct ieee80211_channel *chan = NULL;
	const u8 *bssid = NULL;
	int active_ibss;
	u16 capability;

	active_ibss = ieee80211_sta_active_ibss(sdata);
#ifdef CONFIG_MAC80211_IBSS_DEBUG
	printk(KERN_DEBUG "%s: sta_find_ibss (active_ibss=%d)\n",
	       sdata->dev->name, active_ibss);
#endif /* CONFIG_MAC80211_IBSS_DEBUG */

	if (active_ibss)
		return;

	capability = WLAN_CAPABILITY_IBSS;
	if (ifibss->privacy)
		capability |= WLAN_CAPABILITY_PRIVACY;
	if (ifibss->fixed_bssid)
		bssid = ifibss->bssid;
	if (ifibss->fixed_channel)
		chan = ifibss->channel;
	if (!is_zero_ether_addr(ifibss->bssid))
		bssid = ifibss->bssid;
	bss = (void *)cfg80211_get_bss(local->hw.wiphy, chan, bssid,
				       ifibss->ssid, ifibss->ssid_len,
				       WLAN_CAPABILITY_IBSS |
				       WLAN_CAPABILITY_PRIVACY,
				       capability);

	if (bss) {
#ifdef CONFIG_MAC80211_IBSS_DEBUG
		printk(KERN_DEBUG "   sta_find_ibss: selected %pM current "
		       "%pM\n", bss->cbss.bssid, ifibss->bssid);
#endif /* CONFIG_MAC80211_IBSS_DEBUG */

		printk(KERN_DEBUG "%s: Selected IBSS BSSID %pM"
		       " based on configured SSID\n",
		       sdata->dev->name, bss->cbss.bssid);

		ieee80211_sta_join_ibss(sdata, bss);
		ieee80211_rx_bss_put(local, bss);
		return;
	}

#ifdef CONFIG_MAC80211_IBSS_DEBUG
	printk(KERN_DEBUG "   did not try to join ibss\n");
#endif /* CONFIG_MAC80211_IBSS_DEBUG */

	/* Selected IBSS not found in current scan results - try to scan */
	if (ifibss->state == IEEE80211_IBSS_MLME_JOINED &&
	    !ieee80211_sta_active_ibss(sdata)) {
		mod_timer(&ifibss->timer,
			  round_jiffies(jiffies + IEEE80211_IBSS_MERGE_INTERVAL));
	} else if (time_after(jiffies, ifibss->last_scan_completed +
					IEEE80211_SCAN_INTERVAL)) {
		printk(KERN_DEBUG "%s: Trigger new scan to find an IBSS to "
		       "join\n", sdata->dev->name);

		ieee80211_request_internal_scan(sdata, ifibss->ssid,
						ifibss->ssid_len);
	} else if (ifibss->state != IEEE80211_IBSS_MLME_JOINED) {
		int interval = IEEE80211_SCAN_INTERVAL;

		if (time_after(jiffies, ifibss->ibss_join_req +
			       IEEE80211_IBSS_JOIN_TIMEOUT)) {
			if (!(local->oper_channel->flags & IEEE80211_CHAN_NO_IBSS)) {
				ieee80211_sta_create_ibss(sdata);
				return;
			}
			printk(KERN_DEBUG "%s: IBSS not allowed on"
			       " %d MHz\n", sdata->dev->name,
			       local->hw.conf.channel->center_freq);

			/* No IBSS found - decrease scan interval and continue
			 * scanning. */
			interval = IEEE80211_SCAN_INTERVAL_SLOW;
		}

		ifibss->state = IEEE80211_IBSS_MLME_SEARCH;
		mod_timer(&ifibss->timer,
			  round_jiffies(jiffies + interval));
	}
}

static void ieee80211_rx_mgmt_probe_req(struct ieee80211_sub_if_data *sdata,
					struct ieee80211_mgmt *mgmt,
					size_t len)
{
	struct ieee80211_if_ibss *ifibss = &sdata->u.ibss;
	struct ieee80211_local *local = sdata->local;
	int tx_last_beacon;
	struct sk_buff *skb;
	struct ieee80211_mgmt *resp;
	u8 *pos, *end;

	if (ifibss->state != IEEE80211_IBSS_MLME_JOINED ||
	    len < 24 + 2 || !ifibss->presp)
		return;

	tx_last_beacon = drv_tx_last_beacon(local);

#ifdef CONFIG_MAC80211_IBSS_DEBUG
	printk(KERN_DEBUG "%s: RX ProbeReq SA=%pM DA=%pM BSSID=%pM"
	       " (tx_last_beacon=%d)\n",
	       sdata->dev->name, mgmt->sa, mgmt->da,
	       mgmt->bssid, tx_last_beacon);
#endif /* CONFIG_MAC80211_IBSS_DEBUG */

	if (!tx_last_beacon)
		return;

	if (memcmp(mgmt->bssid, ifibss->bssid, ETH_ALEN) != 0 &&
	    memcmp(mgmt->bssid, "\xff\xff\xff\xff\xff\xff", ETH_ALEN) != 0)
		return;

	end = ((u8 *) mgmt) + len;
	pos = mgmt->u.probe_req.variable;
	if (pos[0] != WLAN_EID_SSID ||
	    pos + 2 + pos[1] > end) {
#ifdef CONFIG_MAC80211_IBSS_DEBUG
		printk(KERN_DEBUG "%s: Invalid SSID IE in ProbeReq "
		       "from %pM\n",
		       sdata->dev->name, mgmt->sa);
#endif
		return;
	}
	if (pos[1] != 0 &&
	    (pos[1] != ifibss->ssid_len ||
	     !memcmp(pos + 2, ifibss->ssid, ifibss->ssid_len))) {
		/* Ignore ProbeReq for foreign SSID */
		return;
	}

	/* Reply with ProbeResp */
	skb = skb_copy(ifibss->presp, GFP_KERNEL);
	if (!skb)
		return;

	resp = (struct ieee80211_mgmt *) skb->data;
	memcpy(resp->da, mgmt->sa, ETH_ALEN);
#ifdef CONFIG_MAC80211_IBSS_DEBUG
	printk(KERN_DEBUG "%s: Sending ProbeResp to %pM\n",
	       sdata->dev->name, resp->da);
#endif /* CONFIG_MAC80211_IBSS_DEBUG */
	ieee80211_tx_skb(sdata, skb, 0);
}

static void ieee80211_rx_mgmt_probe_resp(struct ieee80211_sub_if_data *sdata,
					 struct ieee80211_mgmt *mgmt,
					 size_t len,
					 struct ieee80211_rx_status *rx_status)
{
	size_t baselen;
	struct ieee802_11_elems elems;

	if (memcmp(mgmt->da, sdata->dev->dev_addr, ETH_ALEN))
		return; /* ignore ProbeResp to foreign address */

	baselen = (u8 *) mgmt->u.probe_resp.variable - (u8 *) mgmt;
	if (baselen > len)
		return;

	ieee802_11_parse_elems(mgmt->u.probe_resp.variable, len - baselen,
				&elems);

	ieee80211_rx_bss_info(sdata, mgmt, len, rx_status, &elems, false);
}

static void ieee80211_rx_mgmt_beacon(struct ieee80211_sub_if_data *sdata,
				     struct ieee80211_mgmt *mgmt,
				     size_t len,
				     struct ieee80211_rx_status *rx_status)
{
	size_t baselen;
	struct ieee802_11_elems elems;

	/* Process beacon from the current BSS */
	baselen = (u8 *) mgmt->u.beacon.variable - (u8 *) mgmt;
	if (baselen > len)
		return;

	ieee802_11_parse_elems(mgmt->u.beacon.variable, len - baselen, &elems);

	ieee80211_rx_bss_info(sdata, mgmt, len, rx_status, &elems, true);
}

static void ieee80211_ibss_rx_queued_mgmt(struct ieee80211_sub_if_data *sdata,
					  struct sk_buff *skb)
{
	struct ieee80211_rx_status *rx_status;
	struct ieee80211_mgmt *mgmt;
	u16 fc;

	rx_status = IEEE80211_SKB_RXCB(skb);
	mgmt = (struct ieee80211_mgmt *) skb->data;
	fc = le16_to_cpu(mgmt->frame_control);

	switch (fc & IEEE80211_FCTL_STYPE) {
	case IEEE80211_STYPE_PROBE_REQ:
		ieee80211_rx_mgmt_probe_req(sdata, mgmt, skb->len);
		break;
	case IEEE80211_STYPE_PROBE_RESP:
		ieee80211_rx_mgmt_probe_resp(sdata, mgmt, skb->len,
					     rx_status);
		break;
	case IEEE80211_STYPE_BEACON:
		ieee80211_rx_mgmt_beacon(sdata, mgmt, skb->len,
					 rx_status);
		break;
	case IEEE80211_STYPE_AUTH:
		ieee80211_rx_mgmt_auth_ibss(sdata, mgmt, skb->len);
		break;
	}

	kfree_skb(skb);
}

static void ieee80211_ibss_work(void *s)
{
	struct ieee80211_sub_if_data *sdata = s;
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_ibss *ifibss;
	struct sk_buff *skb;

	if (WARN_ON(local->suspended))
		return;

	if (!netif_running(sdata->dev))
		return;

	if (local->scanning)
		return;

	if (WARN_ON(sdata->vif.type != NL80211_IFTYPE_ADHOC))
		return;
	ifibss = &sdata->u.ibss;

	while ((skb = skb_dequeue(&ifibss->skb_queue)))
		ieee80211_ibss_rx_queued_mgmt(sdata, skb);

	if (!test_and_clear_bit(IEEE80211_IBSS_REQ_RUN, &ifibss->request))
		return;

	switch (ifibss->state) {
	case IEEE80211_IBSS_MLME_SEARCH:
		ieee80211_sta_find_ibss(sdata);
		break;
	case IEEE80211_IBSS_MLME_JOINED:
		ieee80211_sta_merge_ibss(sdata);
		break;
	default:
		WARN_ON(1);
		break;
	}
}

static void ieee80211_ibss_timer(unsigned long data)
{
	struct ieee80211_sub_if_data *sdata =
		(struct ieee80211_sub_if_data *) data;
	struct ieee80211_if_ibss *ifibss = &sdata->u.ibss;
	struct ieee80211_local *local = sdata->local;

	if (local->quiescing) {
		ifibss->timer_running = true;
		return;
	}

	set_bit(IEEE80211_IBSS_REQ_RUN, &ifibss->request);
	ieee80211_queue_work(&local->hw, &ifibss->work);
}

#ifdef CONFIG_PM
void ieee80211_ibss_quiesce(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_ibss *ifibss = &sdata->u.ibss;

#if 0 /* Not in RHEL5... */
	cancel_work_sync(&ifibss->work);
#else
	ieee80211_cancel_work(&local->hw, &ifibss->work);
#endif
	if (del_timer_sync(&ifibss->timer))
		ifibss->timer_running = true;
}

void ieee80211_ibss_restart(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_if_ibss *ifibss = &sdata->u.ibss;

	if (ifibss->timer_running) {
		add_timer(&ifibss->timer);
		ifibss->timer_running = false;
	}
}
#endif

void ieee80211_ibss_setup_sdata(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_if_ibss *ifibss = &sdata->u.ibss;

	INIT_WORK(&ifibss->work, ieee80211_ibss_work, sdata);
	setup_timer(&ifibss->timer, ieee80211_ibss_timer,
		    (unsigned long) sdata);
	skb_queue_head_init(&ifibss->skb_queue);
}

/* scan finished notification */
void ieee80211_ibss_notify_scan_completed(struct ieee80211_local *local)
{
	struct ieee80211_sub_if_data *sdata;

	mutex_lock(&local->iflist_mtx);
	list_for_each_entry(sdata, &local->interfaces, list) {
		if (!netif_running(sdata->dev))
			continue;
		if (sdata->vif.type != NL80211_IFTYPE_ADHOC)
			continue;
		if (!sdata->u.ibss.ssid_len)
			continue;
		sdata->u.ibss.last_scan_completed = jiffies;
		mod_timer(&sdata->u.ibss.timer, 0);
	}
	mutex_unlock(&local->iflist_mtx);
}

ieee80211_rx_result
ieee80211_ibss_rx_mgmt(struct ieee80211_sub_if_data *sdata, struct sk_buff *skb)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_mgmt *mgmt;
	u16 fc;

	if (skb->len < 24)
		return RX_DROP_MONITOR;

	mgmt = (struct ieee80211_mgmt *) skb->data;
	fc = le16_to_cpu(mgmt->frame_control);

	switch (fc & IEEE80211_FCTL_STYPE) {
	case IEEE80211_STYPE_PROBE_RESP:
	case IEEE80211_STYPE_BEACON:
	case IEEE80211_STYPE_PROBE_REQ:
	case IEEE80211_STYPE_AUTH:
		skb_queue_tail(&sdata->u.ibss.skb_queue, skb);
		ieee80211_queue_work(&local->hw, &sdata->u.ibss.work);
		return RX_QUEUED;
	}

	return RX_DROP_MONITOR;
}

int ieee80211_ibss_join(struct ieee80211_sub_if_data *sdata,
			struct cfg80211_ibss_params *params)
{
	struct sk_buff *skb;

	if (params->bssid) {
		memcpy(sdata->u.ibss.bssid, params->bssid, ETH_ALEN);
		sdata->u.ibss.fixed_bssid = true;
	} else
		sdata->u.ibss.fixed_bssid = false;

	sdata->u.ibss.privacy = params->privacy;

	sdata->vif.bss_conf.beacon_int = params->beacon_interval;

	sdata->u.ibss.channel = params->channel;
	sdata->u.ibss.fixed_channel = params->channel_fixed;

	if (params->ie) {
		sdata->u.ibss.ie = kmemdup(params->ie, params->ie_len,
					   GFP_KERNEL);
		if (sdata->u.ibss.ie)
			sdata->u.ibss.ie_len = params->ie_len;
	}

	skb = dev_alloc_skb(sdata->local->hw.extra_tx_headroom +
			    36 /* bitrates */ +
			    34 /* SSID */ +
			    3  /* DS params */ +
			    4  /* IBSS params */ +
			    params->ie_len);
	if (!skb)
		return -ENOMEM;

	sdata->u.ibss.skb = skb;
	sdata->u.ibss.state = IEEE80211_IBSS_MLME_SEARCH;
	sdata->u.ibss.ibss_join_req = jiffies;

	memcpy(sdata->u.ibss.ssid, params->ssid, IEEE80211_MAX_SSID_LEN);

	/*
	 * The ssid_len setting below is used to see whether
	 * we are active, and we need all other settings
	 * before that may get visible.
	 */
	mb();

	sdata->u.ibss.ssid_len = params->ssid_len;

	ieee80211_recalc_idle(sdata->local);

	set_bit(IEEE80211_IBSS_REQ_RUN, &sdata->u.ibss.request);
	ieee80211_queue_work(&sdata->local->hw, &sdata->u.ibss.work);

	return 0;
}

int ieee80211_ibss_leave(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_local *local = sdata->local;
	struct sk_buff *skb;

	del_timer_sync(&sdata->u.ibss.timer);
	clear_bit(IEEE80211_IBSS_REQ_RUN, &sdata->u.ibss.request);
#if 0 /* Not in RHEL5... */
	cancel_work_sync(&sdata->u.ibss.work);
#else
	ieee80211_cancel_work(&local->hw, &sdata->u.ibss.work);
#endif
	clear_bit(IEEE80211_IBSS_REQ_RUN, &sdata->u.ibss.request);

	sta_info_flush(sdata->local, sdata);

	/* remove beacon */
	kfree(sdata->u.ibss.ie);
	skb = sdata->u.ibss.presp;
	rcu_assign_pointer(sdata->u.ibss.presp, NULL);
	ieee80211_bss_info_change_notify(sdata, BSS_CHANGED_BEACON_ENABLED);
	synchronize_rcu();
	kfree_skb(skb);

	skb_queue_purge(&sdata->u.ibss.skb_queue);
	memset(sdata->u.ibss.bssid, 0, ETH_ALEN);
	sdata->u.ibss.ssid_len = 0;

	ieee80211_recalc_idle(sdata->local);

	return 0;
}
