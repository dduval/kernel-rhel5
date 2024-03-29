#if !defined(__MAC80211_DRIVER_TRACE) || defined(TRACE_HEADER_MULTI_READ)
#define __MAC80211_DRIVER_TRACE

#include <linux/tracepoint.h>
#include <net/mac80211.h>
#include "ieee80211_i.h"

#if !defined(CONFIG_MAC80211_DRIVER_API_TRACER) || defined(__CHECKER__)
#undef TRACE_EVENT
#define TRACE_EVENT(name, proto, ...) \
static inline void trace_ ## name(proto) {}
#endif

#undef TRACE_SYSTEM
#define TRACE_SYSTEM mac80211

#define MAXNAME		32
#define LOCAL_ENTRY	__array(char, wiphy_name, 32)
#define LOCAL_ASSIGN	strlcpy(__entry->wiphy_name, wiphy_name(local->hw.wiphy), MAXNAME)
#define LOCAL_PR_FMT	"%s"
#define LOCAL_PR_ARG	__entry->wiphy_name

#define STA_ENTRY	__array(char, sta_addr, ETH_ALEN)
#define STA_ASSIGN	(sta ? memcpy(__entry->sta_addr, sta->addr, ETH_ALEN) : memset(__entry->sta_addr, 0, ETH_ALEN))
#define STA_PR_FMT	" sta:%pM"
#define STA_PR_ARG	__entry->sta_addr

#define VIF_ENTRY	__field(enum nl80211_iftype, vif_type) __field(void *, vif)
#define VIF_ASSIGN	__entry->vif_type = vif ? vif->type : 0; __entry->vif = vif
#define VIF_PR_FMT	" vif:%p(%d)"
#define VIF_PR_ARG	__entry->vif, __entry->vif_type

#if 0 /* Not in RHEL5... */
TRACE_EVENT(drv_start,
	TP_PROTO(struct ieee80211_local *local, int ret),

	TP_ARGS(local, ret),

	TP_STRUCT__entry(
		LOCAL_ENTRY
		__field(int, ret)
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
		__entry->ret = ret;
	),

	TP_printk(
		LOCAL_PR_FMT, LOCAL_PR_ARG
	)
);

TRACE_EVENT(drv_stop,
	TP_PROTO(struct ieee80211_local *local),

	TP_ARGS(local),

	TP_STRUCT__entry(
		LOCAL_ENTRY
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
	),

	TP_printk(
		LOCAL_PR_FMT, LOCAL_PR_ARG
	)
);

TRACE_EVENT(drv_add_interface,
	TP_PROTO(struct ieee80211_local *local,
		 const u8 *addr,
		 struct ieee80211_vif *vif,
		 int ret),

	TP_ARGS(local, addr, vif, ret),

	TP_STRUCT__entry(
		LOCAL_ENTRY
		VIF_ENTRY
		__array(char, addr, 6)
		__field(int, ret)
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
		VIF_ASSIGN;
		memcpy(__entry->addr, addr, 6);
		__entry->ret = ret;
	),

	TP_printk(
		LOCAL_PR_FMT  VIF_PR_FMT " addr:%pM ret:%d",
		LOCAL_PR_ARG, VIF_PR_ARG, __entry->addr, __entry->ret
	)
);

TRACE_EVENT(drv_remove_interface,
	TP_PROTO(struct ieee80211_local *local,
		 const u8 *addr, struct ieee80211_vif *vif),

	TP_ARGS(local, addr, vif),

	TP_STRUCT__entry(
		LOCAL_ENTRY
		VIF_ENTRY
		__array(char, addr, 6)
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
		VIF_ASSIGN;
		memcpy(__entry->addr, addr, 6);
	),

	TP_printk(
		LOCAL_PR_FMT  VIF_PR_FMT " addr:%pM",
		LOCAL_PR_ARG, VIF_PR_ARG, __entry->addr
	)
);

TRACE_EVENT(drv_config,
	TP_PROTO(struct ieee80211_local *local,
		 u32 changed,
		 int ret),

	TP_ARGS(local, changed, ret),

	TP_STRUCT__entry(
		LOCAL_ENTRY
		__field(u32, changed)
		__field(int, ret)
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
		__entry->changed = changed;
		__entry->ret = ret;
	),

	TP_printk(
		LOCAL_PR_FMT " ch:%#x ret:%d",
		LOCAL_PR_ARG, __entry->changed, __entry->ret
	)
);

TRACE_EVENT(drv_bss_info_changed,
	TP_PROTO(struct ieee80211_local *local,
		 struct ieee80211_vif *vif,
		 struct ieee80211_bss_conf *info,
		 u32 changed),

	TP_ARGS(local, vif, info, changed),

	TP_STRUCT__entry(
		LOCAL_ENTRY
		VIF_ENTRY
		__field(bool, assoc)
		__field(u16, aid)
		__field(bool, cts)
		__field(bool, shortpre)
		__field(bool, shortslot)
		__field(u8, dtimper)
		__field(u16, bcnint)
		__field(u16, assoc_cap)
		__field(u64, timestamp)
		__field(u32, basic_rates)
		__field(u32, changed)
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
		VIF_ASSIGN;
		__entry->changed = changed;
		__entry->aid = info->aid;
		__entry->assoc = info->assoc;
		__entry->shortpre = info->use_short_preamble;
		__entry->cts = info->use_cts_prot;
		__entry->shortslot = info->use_short_slot;
		__entry->dtimper = info->dtim_period;
		__entry->bcnint = info->beacon_int;
		__entry->assoc_cap = info->assoc_capability;
		__entry->timestamp = info->timestamp;
		__entry->basic_rates = info->basic_rates;
	),

	TP_printk(
		LOCAL_PR_FMT  VIF_PR_FMT " changed:%#x",
		LOCAL_PR_ARG, VIF_PR_ARG, __entry->changed
	)
);

TRACE_EVENT(drv_prepare_multicast,
	TP_PROTO(struct ieee80211_local *local, int mc_count, u64 ret),

	TP_ARGS(local, mc_count, ret),

	TP_STRUCT__entry(
		LOCAL_ENTRY
		__field(int, mc_count)
		__field(u64, ret)
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
		__entry->mc_count = mc_count;
		__entry->ret = ret;
	),

	TP_printk(
		LOCAL_PR_FMT " prepare mc (%d): %llx",
		LOCAL_PR_ARG, __entry->mc_count,
		(unsigned long long) __entry->ret
	)
);

TRACE_EVENT(drv_configure_filter,
	TP_PROTO(struct ieee80211_local *local,
		 unsigned int changed_flags,
		 unsigned int *total_flags,
		 u64 multicast),

	TP_ARGS(local, changed_flags, total_flags, multicast),

	TP_STRUCT__entry(
		LOCAL_ENTRY
		__field(unsigned int, changed)
		__field(unsigned int, total)
		__field(u64, multicast)
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
		__entry->changed = changed_flags;
		__entry->total = *total_flags;
		__entry->multicast = multicast;
	),

	TP_printk(
		LOCAL_PR_FMT " changed:%#x total:%#x",
		LOCAL_PR_ARG, __entry->changed, __entry->total
	)
);

TRACE_EVENT(drv_set_tim,
	TP_PROTO(struct ieee80211_local *local,
		 struct ieee80211_sta *sta, bool set, int ret),

	TP_ARGS(local, sta, set, ret),

	TP_STRUCT__entry(
		LOCAL_ENTRY
		STA_ENTRY
		__field(bool, set)
		__field(int, ret)
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
		STA_ASSIGN;
		__entry->set = set;
		__entry->ret = ret;
	),

	TP_printk(
		LOCAL_PR_FMT STA_PR_FMT " set:%d ret:%d",
		LOCAL_PR_ARG, STA_PR_FMT, __entry->set, __entry->ret
	)
);

TRACE_EVENT(drv_set_key,
	TP_PROTO(struct ieee80211_local *local,
		 enum set_key_cmd cmd, struct ieee80211_vif *vif,
		 struct ieee80211_sta *sta,
		 struct ieee80211_key_conf *key, int ret),

	TP_ARGS(local, cmd, vif, sta, key, ret),

	TP_STRUCT__entry(
		LOCAL_ENTRY
		VIF_ENTRY
		STA_ENTRY
		__field(enum ieee80211_key_alg, alg)
		__field(u8, hw_key_idx)
		__field(u8, flags)
		__field(s8, keyidx)
		__field(int, ret)
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
		VIF_ASSIGN;
		STA_ASSIGN;
		__entry->alg = key->alg;
		__entry->flags = key->flags;
		__entry->keyidx = key->keyidx;
		__entry->hw_key_idx = key->hw_key_idx;
		__entry->ret = ret;
	),

	TP_printk(
		LOCAL_PR_FMT  VIF_PR_FMT  STA_PR_FMT " ret:%d",
		LOCAL_PR_ARG, VIF_PR_ARG, STA_PR_ARG, __entry->ret
	)
);

TRACE_EVENT(drv_update_tkip_key,
	TP_PROTO(struct ieee80211_local *local,
		 struct ieee80211_key_conf *conf,
		 const u8 *address, u32 iv32),

	TP_ARGS(local, conf, address, iv32),

	TP_STRUCT__entry(
		LOCAL_ENTRY
		__array(u8, addr, 6)
		__field(u32, iv32)
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
		memcpy(__entry->addr, address, 6);
		__entry->iv32 = iv32;
	),

	TP_printk(
		LOCAL_PR_FMT " addr:%pM iv32:%#x",
		LOCAL_PR_ARG, __entry->addr, __entry->iv32
	)
);

TRACE_EVENT(drv_hw_scan,
	TP_PROTO(struct ieee80211_local *local,
		 struct cfg80211_scan_request *req, int ret),

	TP_ARGS(local, req, ret),

	TP_STRUCT__entry(
		LOCAL_ENTRY
		__field(int, ret)
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
		__entry->ret = ret;
	),

	TP_printk(
		LOCAL_PR_FMT " ret:%d",
		LOCAL_PR_ARG, __entry->ret
	)
);

TRACE_EVENT(drv_sw_scan_start,
	TP_PROTO(struct ieee80211_local *local),

	TP_ARGS(local),

	TP_STRUCT__entry(
		LOCAL_ENTRY
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
	),

	TP_printk(
		LOCAL_PR_FMT, LOCAL_PR_ARG
	)
);

TRACE_EVENT(drv_sw_scan_complete,
	TP_PROTO(struct ieee80211_local *local),

	TP_ARGS(local),

	TP_STRUCT__entry(
		LOCAL_ENTRY
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
	),

	TP_printk(
		LOCAL_PR_FMT, LOCAL_PR_ARG
	)
);

TRACE_EVENT(drv_get_stats,
	TP_PROTO(struct ieee80211_local *local,
		 struct ieee80211_low_level_stats *stats,
		 int ret),

	TP_ARGS(local, stats, ret),

	TP_STRUCT__entry(
		LOCAL_ENTRY
		__field(int, ret)
		__field(unsigned int, ackfail)
		__field(unsigned int, rtsfail)
		__field(unsigned int, fcserr)
		__field(unsigned int, rtssucc)
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
		__entry->ret = ret;
		__entry->ackfail = stats->dot11ACKFailureCount;
		__entry->rtsfail = stats->dot11RTSFailureCount;
		__entry->fcserr = stats->dot11FCSErrorCount;
		__entry->rtssucc = stats->dot11RTSSuccessCount;
	),

	TP_printk(
		LOCAL_PR_FMT " ret:%d",
		LOCAL_PR_ARG, __entry->ret
	)
);

TRACE_EVENT(drv_get_tkip_seq,
	TP_PROTO(struct ieee80211_local *local,
		 u8 hw_key_idx, u32 *iv32, u16 *iv16),

	TP_ARGS(local, hw_key_idx, iv32, iv16),

	TP_STRUCT__entry(
		LOCAL_ENTRY
		__field(u8, hw_key_idx)
		__field(u32, iv32)
		__field(u16, iv16)
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
		__entry->hw_key_idx = hw_key_idx;
		__entry->iv32 = *iv32;
		__entry->iv16 = *iv16;
	),

	TP_printk(
		LOCAL_PR_FMT, LOCAL_PR_ARG
	)
);

TRACE_EVENT(drv_set_rts_threshold,
	TP_PROTO(struct ieee80211_local *local, u32 value, int ret),

	TP_ARGS(local, value, ret),

	TP_STRUCT__entry(
		LOCAL_ENTRY
		__field(u32, value)
		__field(int, ret)
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
		__entry->ret = ret;
		__entry->value = value;
	),

	TP_printk(
		LOCAL_PR_FMT " value:%d ret:%d",
		LOCAL_PR_ARG, __entry->value, __entry->ret
	)
);

TRACE_EVENT(drv_sta_notify,
	TP_PROTO(struct ieee80211_local *local,
		 struct ieee80211_vif *vif,
		 enum sta_notify_cmd cmd,
		 struct ieee80211_sta *sta),

	TP_ARGS(local, vif, cmd, sta),

	TP_STRUCT__entry(
		LOCAL_ENTRY
		VIF_ENTRY
		STA_ENTRY
		__field(u32, cmd)
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
		VIF_ASSIGN;
		STA_ASSIGN;
		__entry->cmd = cmd;
	),

	TP_printk(
		LOCAL_PR_FMT  VIF_PR_FMT  STA_PR_FMT " cmd:%d",
		LOCAL_PR_ARG, VIF_PR_ARG, STA_PR_ARG, __entry->cmd
	)
);

TRACE_EVENT(drv_conf_tx,
	TP_PROTO(struct ieee80211_local *local, u16 queue,
		 const struct ieee80211_tx_queue_params *params,
		 int ret),

	TP_ARGS(local, queue, params, ret),

	TP_STRUCT__entry(
		LOCAL_ENTRY
		__field(u16, queue)
		__field(u16, txop)
		__field(u16, cw_min)
		__field(u16, cw_max)
		__field(u8, aifs)
		__field(int, ret)
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
		__entry->queue = queue;
		__entry->ret = ret;
		__entry->txop = params->txop;
		__entry->cw_max = params->cw_max;
		__entry->cw_min = params->cw_min;
		__entry->aifs = params->aifs;
	),

	TP_printk(
		LOCAL_PR_FMT " queue:%d ret:%d",
		LOCAL_PR_ARG, __entry->queue, __entry->ret
	)
);

TRACE_EVENT(drv_get_tx_stats,
	TP_PROTO(struct ieee80211_local *local,
		 struct ieee80211_tx_queue_stats *stats,
		 int ret),

	TP_ARGS(local, stats, ret),

	TP_STRUCT__entry(
		LOCAL_ENTRY
		__field(int, ret)
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
		__entry->ret = ret;
	),

	TP_printk(
		LOCAL_PR_FMT " ret:%d",
		LOCAL_PR_ARG, __entry->ret
	)
);

TRACE_EVENT(drv_get_tsf,
	TP_PROTO(struct ieee80211_local *local, u64 ret),

	TP_ARGS(local, ret),

	TP_STRUCT__entry(
		LOCAL_ENTRY
		__field(u64, ret)
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
		__entry->ret = ret;
	),

	TP_printk(
		LOCAL_PR_FMT " ret:%llu",
		LOCAL_PR_ARG, (unsigned long long)__entry->ret
	)
);

TRACE_EVENT(drv_set_tsf,
	TP_PROTO(struct ieee80211_local *local, u64 tsf),

	TP_ARGS(local, tsf),

	TP_STRUCT__entry(
		LOCAL_ENTRY
		__field(u64, tsf)
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
		__entry->tsf = tsf;
	),

	TP_printk(
		LOCAL_PR_FMT " tsf:%llu",
		LOCAL_PR_ARG, (unsigned long long)__entry->tsf
	)
);

TRACE_EVENT(drv_reset_tsf,
	TP_PROTO(struct ieee80211_local *local),

	TP_ARGS(local),

	TP_STRUCT__entry(
		LOCAL_ENTRY
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
	),

	TP_printk(
		LOCAL_PR_FMT, LOCAL_PR_ARG
	)
);

TRACE_EVENT(drv_tx_last_beacon,
	TP_PROTO(struct ieee80211_local *local, int ret),

	TP_ARGS(local, ret),

	TP_STRUCT__entry(
		LOCAL_ENTRY
		__field(int, ret)
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
		__entry->ret = ret;
	),

	TP_printk(
		LOCAL_PR_FMT " ret:%d",
		LOCAL_PR_ARG, __entry->ret
	)
);

TRACE_EVENT(drv_ampdu_action,
	TP_PROTO(struct ieee80211_local *local,
		 enum ieee80211_ampdu_mlme_action action,
		 struct ieee80211_sta *sta, u16 tid,
		 u16 *ssn, int ret),

	TP_ARGS(local, action, sta, tid, ssn, ret),

	TP_STRUCT__entry(
		LOCAL_ENTRY
		STA_ENTRY
		__field(u32, action)
		__field(u16, tid)
		__field(u16, ssn)
		__field(int, ret)
	),

	TP_fast_assign(
		LOCAL_ASSIGN;
		STA_ASSIGN;
		__entry->ret = ret;
		__entry->action = action;
		__entry->tid = tid;
		__entry->ssn = *ssn;
	),

	TP_printk(
		LOCAL_PR_FMT  STA_PR_FMT " action:%d tid:%d ret:%d",
		LOCAL_PR_ARG, STA_PR_ARG, __entry->action, __entry->tid, __entry->ret
	)
);
#else
static inline void trace_drv_start(struct ieee80211_local *local, int ret) {}
static inline void trace_drv_stop(struct ieee80211_local *local) {}
static inline void trace_drv_add_interface(struct ieee80211_local *local,
						const u8 *addr,
						struct ieee80211_vif *vif,
						int ret) {}
static inline void trace_drv_remove_interface(struct ieee80211_local *local,
						const u8 *addr,
						struct ieee80211_vif *vif) {}
static inline void trace_drv_config(struct ieee80211_local *local,
					u32 changed,
					int ret) {}
static inline void trace_drv_bss_info_changed(struct ieee80211_local *local,
						struct ieee80211_vif *vif,
						struct ieee80211_bss_conf *info,
						u32 changed) {}
static inline void trace_drv_prepare_multicast(struct ieee80211_local *local,
						int mc_count, u64 ret) {}
static inline void trace_drv_configure_filter(struct ieee80211_local *local,
						unsigned int changed_flags,
						unsigned int *total_flags,
						u64 multicast) {}
static inline void trace_drv_set_tim(struct ieee80211_local *local,
					struct ieee80211_sta *sta,
					bool set, int ret) {}
static inline void trace_drv_set_key(struct ieee80211_local *local,
					enum set_key_cmd cmd,
					struct ieee80211_vif *vif,
					struct ieee80211_sta *sta,
					struct ieee80211_key_conf *key,
					int ret) {}
static inline void trace_drv_update_tkip_key(struct ieee80211_local *local,
						struct ieee80211_key_conf *conf,
						const u8 *address, u32 iv32) {}
static inline void trace_drv_hw_scan(struct ieee80211_local *local,
					struct cfg80211_scan_request *req,
					int ret) {}
static inline void trace_drv_sw_scan_start(struct ieee80211_local *local) {}
static inline void trace_drv_sw_scan_complete(struct ieee80211_local *local) {}
static inline void trace_drv_get_stats(struct ieee80211_local *local,
					struct ieee80211_low_level_stats *stats,
					int ret) {}
static inline void trace_drv_get_tkip_seq(struct ieee80211_local *local,
						u8 hw_key_idx, u32 *iv32,
						u16 *iv16) {}
static inline void trace_drv_set_rts_threshold(struct ieee80211_local *local,
						u32 value, int ret) {}
static inline void trace_drv_sta_notify(struct ieee80211_local *local,
					struct ieee80211_vif *vif,
					enum sta_notify_cmd cmd,
					struct ieee80211_sta *sta) {}
static inline void trace_drv_conf_tx(struct ieee80211_local *local, u16 queue,
					const struct ieee80211_tx_queue_params *params,
					int ret) {}
static inline void trace_drv_get_tx_stats(struct ieee80211_local *local,
						struct ieee80211_tx_queue_stats *stats,
						int ret) {}
static inline void trace_drv_get_tsf(struct ieee80211_local *local, u64 ret) {}
static inline void trace_drv_set_tsf(struct ieee80211_local *local, u64 tsf) {}
static inline void trace_drv_reset_tsf(struct ieee80211_local *local) {}
static inline void trace_drv_tx_last_beacon(struct ieee80211_local *local,
						int ret) {}
static inline void trace_drv_ampdu_action(struct ieee80211_local *local,
						enum ieee80211_ampdu_mlme_action action,
						struct ieee80211_sta *sta,
						u16 tid, u16 *ssn, int ret) {}
#endif
#endif /* !__MAC80211_DRIVER_TRACE || TRACE_HEADER_MULTI_READ */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE driver-trace
#if 0 /* Not in RHEL5... */
#include <trace/define_trace.h>
#endif
