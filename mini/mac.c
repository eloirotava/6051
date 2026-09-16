// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SSV6051 mac80211 glue: capabilities and callbacks (station mode).
 */
#include <linux/etherdevice.h>

#include "ssv6051.h"

#define CHAN(_ch, _freq) { .band = NL80211_BAND_2GHZ, .center_freq = (_freq), \
			   .hw_value = (_ch), .max_power = 20 }

static struct ieee80211_channel ssv_channels[] = {
	CHAN(1, 2412), CHAN(2, 2417), CHAN(3, 2422), CHAN(4, 2427),
	CHAN(5, 2432), CHAN(6, 2437), CHAN(7, 2442), CHAN(8, 2447),
	CHAN(9, 2452), CHAN(10, 2457), CHAN(11, 2462), CHAN(12, 2467),
	CHAN(13, 2472), CHAN(14, 2484),
};

static struct ieee80211_rate ssv_bitrates[] = {
	{ .bitrate = 10, .hw_value = 0 },
	{ .bitrate = 20, .hw_value = 1, .hw_value_short = 4,
	  .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 55, .hw_value = 2, .hw_value_short = 5,
	  .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 110, .hw_value = 3, .hw_value_short = 6,
	  .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 60, .hw_value = 7 },
	{ .bitrate = 90, .hw_value = 8 },
	{ .bitrate = 120, .hw_value = 9 },
	{ .bitrate = 180, .hw_value = 10 },
	{ .bitrate = 240, .hw_value = 11 },
	{ .bitrate = 360, .hw_value = 12 },
	{ .bitrate = 480, .hw_value = 13 },
	{ .bitrate = 540, .hw_value = 14 },
};

static int ssv_start(struct ieee80211_hw *hw)
{
	struct ssv_dev *sd = hw->priv;
	int ret;

	mutex_lock(&sd->mutex);
	ret = ssv_hw_start(sd);
	mutex_unlock(&sd->mutex);
	if (ret)
		dev_err(sd->dev, "start failed: %d\n", ret);
	return ret;
}

static void ssv_stop(struct ieee80211_hw *hw, bool suspend)
{
	struct ssv_dev *sd = hw->priv;

	mutex_lock(&sd->mutex);
	ssv_hw_stop(sd);
	ssv_tx_flush(sd);
	mutex_unlock(&sd->mutex);
}

static int ssv_add_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct ssv_dev *sd = hw->priv;

	if (vif->type != NL80211_IFTYPE_STATION || sd->vif)
		return -EOPNOTSUPP;
	sd->vif = vif;
	return 0;
}

static void ssv_remove_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct ssv_dev *sd = hw->priv;

	if (sd->vif == vif)
		sd->vif = NULL;
}

static int ssv_config(struct ieee80211_hw *hw, int radio_idx, u32 changed)
{
	struct ssv_dev *sd = hw->priv;
	struct ieee80211_channel *chan = hw->conf.chandef.chan;
	int ret = 0;

	if (!(changed & IEEE80211_CONF_CHANGE_CHANNEL) || !chan)
		return 0;
	mutex_lock(&sd->mutex);
	if (chan->hw_value != sd->channel || !sd->started) {
		ieee80211_stop_queues(hw);
		ret = ssv_set_channel(sd, chan->hw_value);
		ieee80211_wake_queues(hw);
	}
	mutex_unlock(&sd->mutex);
	return ret;
}

#define SSV_FILTERS (FIF_ALLMULTI | FIF_BCN_PRBRESP_PROMISC)

static void ssv_configure_filter(struct ieee80211_hw *hw, unsigned int changed,
				 unsigned int *total, u64 multicast)
{
	*total &= SSV_FILTERS;
}

static void ssv_bss_info_changed(struct ieee80211_hw *hw,
				 struct ieee80211_vif *vif,
				 struct ieee80211_bss_conf *info, u64 changed)
{
	struct ssv_dev *sd = hw->priv;

	mutex_lock(&sd->mutex);
	if (changed & BSS_CHANGED_ERP_PREAMBLE)
		sd->short_preamble = info->use_short_preamble;
	if (changed & BSS_CHANGED_BSSID)
		ssv_set_bssid(sd, info->bssid);
	if (changed & BSS_CHANGED_ERP_SLOT)
		ssv_set_slot(sd, info->use_short_slot);
	if (changed & BSS_CHANGED_BASIC_RATES)
		ssv_update_ctrl_rates(sd, info->basic_rates);
	mutex_unlock(&sd->mutex);
}

static int ssv_sta_add(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		       struct ieee80211_sta *sta)
{
	struct ssv_dev *sd = hw->priv;
	struct ssv_sta *ss = (struct ssv_sta *)sta->drv_priv;
	int wsid;

	mutex_lock(&sd->mutex);
	for (wsid = 0; wsid < SSV_NUM_HW_STA; wsid++)
		if (!rcu_access_pointer(sd->sta[wsid]))
			break;
	if (wsid == SSV_NUM_HW_STA) {
		mutex_unlock(&sd->mutex);
		return -ENOSPC;
	}
	ss->wsid = wsid;
	ssv_agg_init(ss);
	spin_lock_bh(&sd->sta_lock);
	ssv_rc_init(sd, sta);
	spin_unlock_bh(&sd->sta_lock);
	ssv_wsid_add(sd, wsid, sta->addr);
	rcu_assign_pointer(sd->sta[wsid], sta);
	mutex_unlock(&sd->mutex);
	return 0;
}

static int ssv_sta_remove(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			  struct ieee80211_sta *sta)
{
	struct ssv_dev *sd = hw->priv;
	struct ssv_sta *ss = (struct ssv_sta *)sta->drv_priv;
	int tid;

	mutex_lock(&sd->mutex);
	if (sd->rx_ba_sta == sta) {
		ssv_rx_ba_session(sd, NULL, 0, 0);
		sd->rx_ba_sta = NULL;
	}
	if (ss->wsid >= 0 && ss->wsid < SSV_NUM_HW_STA &&
	    rcu_access_pointer(sd->sta[ss->wsid]) == sta) {
		RCU_INIT_POINTER(sd->sta[ss->wsid], NULL);
		ssv_wsid_del(sd, ss->wsid);
	}
	ss->wsid = -1;
	mutex_unlock(&sd->mutex);
	synchronize_rcu();
	for (tid = 0; tid < SSV_AGG_TIDS; tid++)
		ssv_agg_flush(sd, ss, tid);
	return 0;
}

static int ssv_conf_tx(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		       unsigned int link_id, u16 ac,
		       const struct ieee80211_tx_queue_params *params)
{
	struct ssv_dev *sd = hw->priv;
	int ret;

	mutex_lock(&sd->mutex);
	ret = ssv_set_edca(sd, ac, vif->bss_conf.qos, params);
	mutex_unlock(&sd->mutex);
	return ret;
}

static void ssv_sw_scan_start(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			      const u8 *mac_addr)
{
	struct ssv_dev *sd = hw->priv;

	mutex_lock(&sd->mutex);
	ssv_scan_cca(sd, true);
	mutex_unlock(&sd->mutex);
}

static void ssv_sw_scan_complete(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct ssv_dev *sd = hw->priv;

	mutex_lock(&sd->mutex);
	ssv_scan_cca(sd, false);
	mutex_unlock(&sd->mutex);
}

static int ssv_ampdu_action(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			    struct ieee80211_ampdu_params *params)
{
	struct ssv_dev *sd = hw->priv;
	int ret = 0;

	mutex_lock(&sd->mutex);
	switch (params->action) {
	case IEEE80211_AMPDU_RX_START:
		/* the MAC tracks a single RX Block Ack session */
		if (sd->rx_ba_sta && (sd->rx_ba_sta != params->sta ||
				      sd->rx_ba_tid != params->tid)) {
			ret = -EBUSY;
			break;
		}
		sd->rx_ba_sta = params->sta;
		sd->rx_ba_tid = params->tid;
		ssv_rx_ba_session(sd, params->sta->addr, params->tid, params->ssn);
		break;
	case IEEE80211_AMPDU_RX_STOP:
		if (sd->rx_ba_sta == params->sta && sd->rx_ba_tid == params->tid) {
			ssv_rx_ba_session(sd, NULL, 0, 0);
			sd->rx_ba_sta = NULL;
		}
		break;
	default:
		ret = ssv_agg_action(sd, vif, params);
	}
	mutex_unlock(&sd->mutex);
	return ret;
}

/* The TX path reads wiphy->rts_threshold directly. */
static int ssv_set_rts_threshold(struct ieee80211_hw *hw, int radio_idx, u32 value)
{
	return 0;
}

static const struct ieee80211_ops ssv_ops = {
	.add_chanctx = ieee80211_emulate_add_chanctx,
	.remove_chanctx = ieee80211_emulate_remove_chanctx,
	.change_chanctx = ieee80211_emulate_change_chanctx,
	.switch_vif_chanctx = ieee80211_emulate_switch_vif_chanctx,
	.wake_tx_queue = ieee80211_handle_wake_tx_queue,
	.tx = ssv_tx,
	.start = ssv_start,
	.stop = ssv_stop,
	.add_interface = ssv_add_interface,
	.remove_interface = ssv_remove_interface,
	.config = ssv_config,
	.configure_filter = ssv_configure_filter,
	.bss_info_changed = ssv_bss_info_changed,
	.sta_add = ssv_sta_add,
	.sta_remove = ssv_sta_remove,
	.conf_tx = ssv_conf_tx,
	.set_rts_threshold = ssv_set_rts_threshold,
	.ampdu_action = ssv_ampdu_action,
	.sw_scan_start = ssv_sw_scan_start,
	.sw_scan_complete = ssv_sw_scan_complete,
};

struct ssv_dev *ssv_mac_alloc(struct device *dev)
{
	struct ieee80211_hw *hw;
	struct ssv_dev *sd;

	hw = ieee80211_alloc_hw(sizeof(*sd), &ssv_ops);
	if (!hw)
		return NULL;
	sd = hw->priv;
	sd->hw = hw;
	sd->dev = dev;
	mutex_init(&sd->mutex);
	spin_lock_init(&sd->sta_lock);
	init_waitqueue_head(&sd->cali_wait);
	SET_IEEE80211_DEV(hw, dev);
	return sd;
}

void ssv_mac_free(struct ssv_dev *sd)
{
	ieee80211_free_hw(sd->hw);
}

int ssv_mac_register(struct ssv_dev *sd)
{
	struct ieee80211_hw *hw = sd->hw;
	struct ieee80211_sta_ht_cap *ht = &sd->band.ht_cap;
	int ret;

	ieee80211_hw_set(hw, SIGNAL_DBM);
	ieee80211_hw_set(hw, HAS_RATE_CONTROL);
	ieee80211_hw_set(hw, MFP_CAPABLE);
	ieee80211_hw_set(hw, AMPDU_AGGREGATION);
	hw->max_rx_aggregation_subframes = 16;
	hw->queues = IEEE80211_NUM_ACS;
	hw->extra_tx_headroom = SSV_TX_DESC_LEN;
	hw->max_rates = 1;
	hw->sta_data_size = sizeof(struct ssv_sta);
	hw->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	hw->wiphy->flags &= ~WIPHY_FLAG_PS_ON_BY_DEFAULT;

	sd->band.band = NL80211_BAND_2GHZ;
	sd->band.channels = ssv_channels;
	sd->band.n_channels = ARRAY_SIZE(ssv_channels);
	sd->band.bitrates = ssv_bitrates;
	sd->band.n_bitrates = ARRAY_SIZE(ssv_bitrates);
	/* 1x1, 20 MHz only */
	ht->ht_supported = true;
	ht->cap = IEEE80211_HT_CAP_SGI_20 | IEEE80211_HT_CAP_SM_PS;
	ht->ampdu_factor = IEEE80211_HT_MAX_AMPDU_32K;
	ht->ampdu_density = IEEE80211_HT_MPDU_DENSITY_8;
	ht->mcs.rx_mask[0] = 0xff;
	ht->mcs.rx_highest = cpu_to_le16(72);
	ht->mcs.tx_params = IEEE80211_HT_MCS_TX_DEFINED;
	hw->wiphy->bands[NL80211_BAND_2GHZ] = &sd->band;

	SET_IEEE80211_PERM_ADDR(hw, sd->mac);

	ret = ssv_tx_init(sd);
	if (ret)
		return ret;
	ret = ieee80211_register_hw(hw);
	if (ret) {
		ssv_tx_deinit(sd);
		return ret;
	}
	wiphy_info(hw->wiphy, "SSV6051 ready\n");
	return 0;
}

void ssv_mac_unregister(struct ssv_dev *sd)
{
	ieee80211_unregister_hw(sd->hw);
	ssv_tx_deinit(sd);
}
