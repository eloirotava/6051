// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SSV6051 rate control.
 *
 * The chip retries each frame on its own at the rate in the descriptor and
 * reports counters on request.  Data frames go in windows of RC_WINDOW at
 * one rate; the last frame of a window asks for the report.  One window in
 * RC_PROBE_EVERY probes the next rate up or down.
 * Each rate keeps an EWMA of ACKs per transmission; the rate with the best
 * expected throughput wins.
 */
#include "ssv6051.h"

#define RC_WINDOW		16
#define RC_PROBE_EVERY		4
#define RC_SCALE		1024
#define RC_MIN_PROB		(RC_SCALE / 5)

const struct ssv_rate ssv_rates[SSV_NUM_RATES] = {
	/* CCK long preamble: 1, 2, 5.5, 11 */
	{ 1000, SSV_PHY_CCK, 0, 0 },
	{ 2000, SSV_PHY_CCK, 1, 1 },
	{ 5500, SSV_PHY_CCK, 1, 2 },
	{ 11000, SSV_PHY_CCK, 1, 3 },
	/* CCK short preamble: 2, 5.5, 11 */
	{ 2000, SSV_PHY_CCK, 4, 1 },
	{ 5500, SSV_PHY_CCK, 4, 2 },
	{ 11000, SSV_PHY_CCK, 4, 3 },
	/* OFDM: 6 .. 54 */
	{ 6000, SSV_PHY_OFDM, 7, 4 },
	{ 9000, SSV_PHY_OFDM, 7, 5 },
	{ 12000, SSV_PHY_OFDM, 9, 6 },
	{ 18000, SSV_PHY_OFDM, 9, 7 },
	{ 24000, SSV_PHY_OFDM, 11, 8 },
	{ 36000, SSV_PHY_OFDM, 11, 9 },
	{ 48000, SSV_PHY_OFDM, 11, 10 },
	{ 54000, SSV_PHY_OFDM, 11, 11 },
	/* HT20 MCS0..7, long GI */
	{ 6500, SSV_PHY_HT, 7, 0 },
	{ 13000, SSV_PHY_HT, 9, 1 },
	{ 19500, SSV_PHY_HT, 9, 2 },
	{ 26000, SSV_PHY_HT, 11, 3 },
	{ 39000, SSV_PHY_HT, 11, 4 },
	{ 52000, SSV_PHY_HT, 11, 5 },
	{ 58500, SSV_PHY_HT, 11, 6 },
	{ 65000, SSV_PHY_HT, 11, 7 },
	/* HT20 MCS0..7, short GI */
	{ 7200, SSV_PHY_HT, 7, 0 },
	{ 14400, SSV_PHY_HT, 9, 1 },
	{ 21700, SSV_PHY_HT, 9, 2 },
	{ 28900, SSV_PHY_HT, 11, 3 },
	{ 43300, SSV_PHY_HT, 11, 4 },
	{ 57800, SSV_PHY_HT, 11, 5 },
	{ 65000, SSV_PHY_HT, 11, 6 },
	{ 72200, SSV_PHY_HT, 11, 7 },
};

void ssv_rc_init(struct ssv_dev *sd, struct ieee80211_sta *sta)
{
	struct ssv_sta *ss = (struct ssv_sta *)sta->drv_priv;
	struct ssv_rc *rc = &ss->rc;
	const struct ieee80211_sta_ht_cap *ht = &sta->deflink.ht_cap;
	u32 legacy = sta->deflink.supp_rates[NL80211_BAND_2GHZ];
	int i;

	memset(rc, 0, sizeof(*rc));
	if (ht->ht_supported && (ht->mcs.rx_mask[0] & 1)) {
		u8 base = (ht->cap & IEEE80211_HT_CAP_SGI_20) ?
			  SSV_RATE_MCS_SGI : SSV_RATE_MCS_LGI;

		for (i = 0; i < 8; i++)
			if (ht->mcs.rx_mask[0] & BIT(i))
				rc->rate[rc->n++] = base + i;
	} else {
		/* ascending speed: 1, 2, 5.5, 6, 9, 11, 12 .. 54 */
		static const u8 order[] = { 0, 1, 2, 4, 5, 3, 6, 7, 8, 9, 10, 11 };

		for (i = 0; i < ARRAY_SIZE(order); i++) {
			int b = order[i];

			if (legacy & BIT(b))
				rc->rate[rc->n++] = b < 4 ? b : SSV_RATE_OFDM + b - 4;
		}
	}
	if (!rc->n)
		rc->rate[rc->n++] = 0;

	/* start in the lower middle; probing climbs from there */
	rc->cur = rc->n / 3;
	for (i = 0; i <= rc->cur; i++)
		rc->prob[i] = RC_SCALE / 2;
}

u8 ssv_rc_get(struct ssv_dev *sd, struct ssv_sta *ss, bool *report)
{
	struct ssv_rc *rc = &ss->rc;
	u8 rate;

	spin_lock_bh(&sd->sta_lock);
	if (!rc->win_left) {
		/*
		 * New window.  Every RC_PROBE_EVERY-th one probes a neighbour,
		 * alternating up and down, so both stay fresh.
		 */
		rc->windows++;
		rc->win_idx = rc->cur;
		if (rc->windows % RC_PROBE_EVERY == 0) {
			bool up = (rc->windows / RC_PROBE_EVERY) & 1;

			if (up && rc->cur + 1 < rc->n)
				rc->win_idx = rc->cur + 1;
			else if (!up && rc->cur > 0)
				rc->win_idx = rc->cur - 1;
		}
		rc->win_left = RC_WINDOW;
	}
	rc->win_left--;
	*report = !rc->win_left;
	rate = rc->rate[rc->win_idx];
	spin_unlock_bh(&sd->sta_lock);
	return rate;
}

static void ssv_rc_select(struct ssv_rc *rc)
{
	u32 best_tp = 0;
	int i, best = rc->cur;

	for (i = 0; i < rc->n; i++) {
		u32 tp = rc->prob[i] * ssv_rates[rc->rate[i]].kbps / 64;

		if (rc->prob[i] >= RC_MIN_PROB && tp > best_tp) {
			best_tp = tp;
			best = i;
		}
	}
	/* nothing works well: step down */
	if (!best_tp && rc->cur > 0)
		best = rc->cur - 1;
	rc->cur = best;
}

/*
 * The firmware accumulates, per station, the frames sent since the last
 * report (ampdu_len), how many were acknowledged (ampdu_ack_len) and the
 * transmissions it took at the reported rate (count), and reports when a
 * frame asking for it completes.  The driver keeps the rate constant for a
 * whole window, so the numbers belong to that rate.
 */
void ssv_rc_report(struct ssv_dev *sd, const struct ssv_rc_report *rpt)
{
	struct ieee80211_sta *sta;
	struct ssv_rc *rc;
	int rate = rpt->rates[0].data_rate;
	int i;

	if (rpt->wsid >= SSV_NUM_STA || rate < 0 || !rpt->rates[0].count)
		return;
	/* reports name the long-preamble CCK rate */
	if (rate > 3 && rate < SSV_RATE_OFDM)
		rate -= 3;

	rcu_read_lock();
	sta = rcu_dereference(sd->sta[rpt->wsid]);
	if (!sta)
		goto out;
	rc = &((struct ssv_sta *)sta->drv_priv)->rc;

	spin_lock_bh(&sd->sta_lock);
	for (i = 0; i < rc->n; i++) {
		u8 r = rc->rate[i];
		u32 p;

		if (r > 3 && r < SSV_RATE_OFDM)
			r -= 3;
		if (r != rate)
			continue;
		p = min_t(u32, rpt->ampdu_ack_len, rpt->rates[0].count) * RC_SCALE /
		    rpt->rates[0].count;
		rc->prob[i] = rc->sampled[i] ? (rc->prob[i] * 3 + p) / 4 : p;
		rc->sampled[i] = true;
		ssv_rc_select(rc);
		dev_dbg(sd->dev, "rc: rate %u %u/%u/%u -> p %u, cur %u (rate %u)\n",
			r, rpt->ampdu_ack_len, rpt->ampdu_len, rpt->rates[0].count,
			rc->prob[i], rc->cur, rc->rate[rc->cur]);
		break;
	}
	spin_unlock_bh(&sd->sta_lock);
out:
	rcu_read_unlock();
}

/*
 * Rate chain for an aggregate: the current rate and two below it (the
 * chip falls back along the chain).  Every RC_PROBE_EVERY-th aggregate
 * starts one step up or down instead.  Only for HT peers.
 */
bool ssv_rc_agg_chain(struct ssv_dev *sd, struct ssv_sta *ss, u8 *chain)
{
	struct ssv_rc *rc = &ss->rc;
	int top, i;

	if (!rc->n || rc->rate[0] < SSV_RATE_MCS_LGI)
		return false;
	rc->agg_count++;
	top = rc->cur;
	if (rc->agg_count % RC_PROBE_EVERY == 0) {
		if ((rc->agg_count / RC_PROBE_EVERY) & 1)
			top = min_t(int, top + 1, rc->n - 1);
		else
			top = max_t(int, top - 1, 0);
	}
	for (i = 0; i < SSV_TX_MAX_RATES; i++)
		chain[i] = rc->rate[max_t(int, top - i, 0)];
	return true;
}

/*
 * Aggregate outcome: @acked of @frames MPDUs got through; @tries is the
 * number of attempts the chip made at the first rate.
 */
void ssv_rc_agg_result(struct ssv_dev *sd, struct ssv_sta *ss, u8 rate,
		       int frames, int acked, int tries)
{
	struct ssv_rc *rc = &ss->rc;
	u32 p;
	int i;

	if (!frames)
		return;
	for (i = 0; i < rc->n; i++) {
		if (rc->rate[i] != rate)
			continue;
		p = min(acked, frames) * RC_SCALE / (frames * max(tries, 1));
		rc->prob[i] = rc->sampled[i] ? (rc->prob[i] * 3 + p) / 4 : p;
		rc->sampled[i] = true;
		ssv_rc_select(rc);
		break;
	}
}
