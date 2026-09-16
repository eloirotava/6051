// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SSV6051 rate control.
 *
 * The chip retries each frame on its own at the rate in the descriptor and
 * only reports the outcome for frames that ask for it (attempts and
 * whether an ACK came back).  About one data frame in RC_REPORT_EVERY asks,
 * and one in RC_PROBE_EVERY is sent one rate higher, so a better rate can
 * be discovered.  Each rate keeps an EWMA of its success probability; the
 * rate with the best expected throughput wins.
 */
#include "ssv6051.h"

#define RC_REPORT_EVERY		8
#define RC_PROBE_EVERY		32
#define RC_INTERVAL		msecs_to_jiffies(100)
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
		rc->prob[i] = RC_SCALE * 3 / 4;
	rc->last_update = jiffies;
}

u8 ssv_rc_get(struct ssv_dev *sd, struct ssv_sta *ss, bool *report)
{
	struct ssv_rc *rc = &ss->rc;
	u8 idx, rate;

	spin_lock_bh(&sd->sta_lock);
	idx = rc->cur;
	rc->frames++;
	if (rc->frames % RC_PROBE_EVERY == 0 && idx + 1 < rc->n) {
		idx++;
		*report = true;
	} else {
		*report = rc->frames % RC_REPORT_EVERY == 0;
	}
	rate = rc->rate[idx];
	spin_unlock_bh(&sd->sta_lock);
	return rate;
}

static void ssv_rc_update(struct ssv_rc *rc)
{
	u32 best_tp = 0;
	int i, best = rc->cur;

	for (i = 0; i < rc->n; i++) {
		if (rc->att[i]) {
			u32 p = min_t(u32, rc->ok[i], rc->att[i]) * RC_SCALE / rc->att[i];

			rc->prob[i] = rc->prob[i] ? (rc->prob[i] * 3 + p) / 4 : p;
			rc->att[i] = rc->ok[i] = 0;
		}
	}
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

void ssv_rc_report(struct ssv_dev *sd, const struct ssv_rc_report *rpt)
{
	struct ieee80211_sta *sta;
	struct ssv_rc *rc;
	int rate = rpt->rates[0].data_rate;
	int i;

	if (rpt->wsid >= SSV_NUM_HW_STA || rate < 0)
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

		if (r > 3 && r < SSV_RATE_OFDM)
			r -= 3;
		if (r == rate) {
			rc->att[i] += max_t(u8, rpt->rates[0].count, 1);
			rc->ok[i] += rpt->ampdu_ack_len ? 1 : 0;
			break;
		}
	}
	if (time_after(jiffies, rc->last_update + RC_INTERVAL)) {
		ssv_rc_update(rc);
		rc->last_update = jiffies;
	}
	spin_unlock_bh(&sd->sta_lock);
out:
	rcu_read_unlock();
}
