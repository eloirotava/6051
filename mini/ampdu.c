// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SSV6051 A-MPDU transmit.
 *
 * The host builds the aggregate (delimiter + MPDU + FCS placeholder +
 * padding, the chip fills in the CRC) and hands it over with a chain of
 * three rates.  The chip transmits it (retrying with the chain), and
 * forwards the peer's Block Ack with a note of which sequence numbers it
 * carried, or a NO_BA event when nothing came back.  MPDUs missing from
 * the bitmap are resent in the next aggregate, up to AGG_MAX_TRIES.
 *
 * Several aggregates of a TID can be in flight, all inside the peer's
 * window starting at the oldest unfinished MPDU.  The note lists the
 * sequence numbers the chip put in the aggregate, which ties each Block Ack
 * to its MPDUs.
 */
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/unaligned.h>

#include "ssv6051.h"

#define AGG_MAX_TRIES		4
#define AGG_MAX_FRAMES		16
#define AGG_MAX_INFLIGHT	3
#define AGG_BA_TIMEOUT		msecs_to_jiffies(200)
#define AGG_DELIM_LEN		4
#define AGG_FCS_LEN		4
#define AGG_SIGNATURE		0x4e
#define AGG_MPDU_NAV		48
#define AGG_MAX_BYTES		(((HW_TX_PAGES / 2) << HW_PAGE_SHIFT) - TX_ALLOC_RSVD)
#define BA_LEN			32
#define SEQ_MASK		0xfff

/* per-MPDU state while the driver owns it, in the tx_info status area */
struct ssv_agg_cb {
	u32 sent_at;
	u8 rate;
	u8 id;
} __packed;

/* Longest aggregate per HT rate (15..30), from the vendor driver */
static const u16 agg_max_len[16] = {
	4600, 9200, 13800, 18500, 27700, 37000, 41600, 46200,
	5100, 10200, 15400, 20500, 30800, 41100, 46200, 51300,
};

/* Appended by the firmware to a forwarded Block Ack, and the NO_BA body */
struct ssv_ba_note {
	u8 wsid;
	struct ssv_tx_rate_rpt tried[SSV_TX_MAX_RATES];
	__le16 seq[24];
} __packed;

struct ssv_ba_frame {
	__le16 frame_control;
	__le16 duration;
	u8 ra[ETH_ALEN];
	u8 ta[ETH_ALEN];
	__le16 control;		/* TID in bits 15..12 */
	__le16 ssc;		/* starting sequence << 4 */
	__le32 bitmap[2];
} __packed;

static u16 skb_seq(struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;

	return le16_to_cpu(hdr->seq_ctrl) >> 4;
}

static struct ssv_agg_cb *agg_cb(struct sk_buff *skb)
{
	BUILD_BUG_ON(sizeof(struct ssv_agg_cb) >
		     sizeof(IEEE80211_SKB_CB(skb)->status.status_driver_data));
	return (struct ssv_agg_cb *)IEEE80211_SKB_CB(skb)->status.status_driver_data;
}

/* true if sequence number @a comes before @b */
static bool seq_before(u16 a, u16 b)
{
	u16 d = (b - a) & SEQ_MASK;

	return d && d < 2048;
}

static u8 skb_tid(struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;

	return *ieee80211_get_qos_ctl(hdr) & IEEE80211_QOS_CTL_TID_MASK;
}

static u8 delim_half_crc(u8 v)
{
	u32 c = v, x = v;

	c ^= (x >> 1) | (x << 7);
	c ^= x >> 2;
	if (x & 2)
		c ^= 0xc0;
	c ^= (x << 4) & 0x30;
	return c;
}

static u8 delim_crc(const u8 *p)
{
	u8 crc = 0xcf;

	crc ^= delim_half_crc(p[0]);
	crc = delim_half_crc(crc) ^ delim_half_crc(p[1]);
	return ~crc;
}

static size_t agg_mpdu_size(struct sk_buff *skb)
{
	return round_up(AGG_DELIM_LEN + skb->len + AGG_FCS_LEN, 4);
}

void ssv_agg_init(struct ssv_sta *ss)
{
	int t;

	for (t = 0; t < SSV_AGG_TIDS; t++) {
		struct ssv_agg *a = &ss->agg[t];

		memset(a, 0, sizeof(*a));
		skb_queue_head_init(&a->q);
		skb_queue_head_init(&a->retry);
		skb_queue_head_init(&a->inflight);
	}
}

static void agg_done(struct ssv_dev *sd, struct sk_buff *skb, bool acked)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);

	ieee80211_tx_info_clear_status(info);
	if (acked)
		info->flags |= IEEE80211_TX_STAT_ACK;
	ieee80211_tx_status_ni(sd->hw, skb);
}

/* Queue an unacknowledged MPDU for resending; give up after AGG_MAX_TRIES. */
static bool agg_retry(struct ssv_agg *a, struct sk_buff *skb,
		      struct sk_buff_head *dropped)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	u16 seq = skb_seq(skb);
	u8 *tries = &a->tries[seq & (SSV_AGG_WINDOW - 1)];
	struct sk_buff *pos;

	if (++*tries >= AGG_MAX_TRIES) {
		*tries = 0;
		__skb_queue_tail(dropped, skb);
		return true;
	}
	hdr->frame_control |= cpu_to_le16(IEEE80211_FCTL_RETRY);
	skb_queue_walk(&a->retry, pos) {
		if (seq_before(seq, skb_seq(pos))) {
			__skb_queue_before(&a->retry, pos, skb);
			return false;
		}
	}
	__skb_queue_tail(&a->retry, skb);
	return false;
}

static void agg_acked(struct ssv_agg *a, struct sk_buff *skb,
		      struct sk_buff_head *done)
{
	a->tries[skb_seq(skb) & (SSV_AGG_WINDOW - 1)] = 0;
	__skb_queue_tail(done, skb);
}

/* Oldest MPDU not finished yet: the start of the window we may use. */
static bool agg_window_start(struct ssv_agg *a, u16 *start)
{
	struct sk_buff *skb;
	bool found = false;

	skb = skb_peek(&a->retry);
	if (skb) {
		*start = skb_seq(skb);
		found = true;
	}
	skb_queue_walk(&a->inflight, skb) {
		if (!found || seq_before(skb_seq(skb), *start)) {
			*start = skb_seq(skb);
			found = true;
		}
	}
	return found;
}

static int agg_inflight_count(struct ssv_agg *a)
{
	struct sk_buff *skb;
	int n = 0, last = -1;

	skb_queue_walk(&a->inflight, skb) {
		if (agg_cb(skb)->id != last) {
			last = agg_cb(skb)->id;
			n++;
		}
	}
	return n;
}

static void agg_complete(struct ssv_dev *sd, struct sk_buff_head *q, bool acked)
{
	struct sk_buff *skb;

	while ((skb = __skb_dequeue(q)))
		agg_done(sd, skb, acked);
}

/* Drop all frames of a TID (session torn down). */
void ssv_agg_flush(struct ssv_dev *sd, struct ssv_sta *ss, u8 tid)
{
	struct ssv_agg *a = &ss->agg[tid];
	struct sk_buff_head drop;

	__skb_queue_head_init(&drop);
	spin_lock_bh(&sd->sta_lock);
	a->state = SSV_AGG_OFF;
	skb_queue_splice_tail_init(&a->inflight, &drop);
	skb_queue_splice_tail_init(&a->retry, &drop);
	atomic_sub(skb_queue_len(&a->q), &sd->agg_queued);
	skb_queue_splice_tail_init(&a->q, &drop);
	spin_unlock_bh(&sd->sta_lock);
	agg_complete(sd, &drop, false);
}

/* Called from ssv_tx(); returns true if the frame was taken. */
bool ssv_agg_tx(struct ssv_dev *sd, struct ieee80211_sta *sta, struct sk_buff *skb)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	struct ssv_sta *ss = (struct ssv_sta *)sta->drv_priv;
	struct ssv_agg *a;
	bool taken = false;
	u8 tid;

	if (!sta->deflink.ht_cap.ht_supported ||
	    !ieee80211_is_data_qos(hdr->frame_control) ||
	    skb->protocol == cpu_to_be16(ETH_P_PAE) ||
	    is_multicast_ether_addr(hdr->addr1))
		return false;

	tid = skb_tid(skb);
	a = &ss->agg[tid];

	spin_lock_bh(&sd->sta_lock);
	if (a->state == SSV_AGG_OPERATIONAL && (info->flags & IEEE80211_TX_CTL_AMPDU)) {
		__skb_queue_tail(&a->q, skb);
		atomic_inc(&sd->agg_queued);
		taken = true;
	} else if (a->state == SSV_AGG_OFF &&
		   time_after(jiffies, a->retry_start)) {
		a->state = SSV_AGG_STARTING;
		a->retry_start = jiffies + 10 * HZ;
		spin_unlock_bh(&sd->sta_lock);
		if (ieee80211_start_tx_ba_session(sta, tid, 0)) {
			spin_lock_bh(&sd->sta_lock);
			a->state = SSV_AGG_OFF;
		} else {
			spin_lock_bh(&sd->sta_lock);
		}
	}
	spin_unlock_bh(&sd->sta_lock);
	return taken;
}

static void agg_set_timing(struct ssv_tx_desc *d, struct ssv_rc_retry *rc,
			   u8 rate, u32 len)
{
	const struct ssv_rate *r = &ssv_rates[rate];
	const struct ssv_rate *c = &ssv_rates[r->ctrl];
	bool sgi = rate >= SSV_RATE_MCS_SGI;
	u32 frame, ack, nav, consume, l;

	frame = ssv_ht_airtime(r->dot11, len, sgi);
	ack = ssv_legacy_airtime(c, BA_LEN, false);
	/* aggregates always go with RTS/CTS */
	nav = frame + ack + ssv_legacy_airtime(c, 14, false);
	consume = nav + ssv_legacy_airtime(c, 20, false);
	l = frame - 10;
	l = ((l - (6 + 20)) + 3) >> 2;

	rc->drate = rate;
	rc->crate = r->ctrl;
	rc->rts_cts_nav = nav;
	rc->frame_consume_time = (consume >> 5) + 1;
	rc->dl_length = l + (l << 1) - 3;
}

/*
 * Build one aggregate for @a into sd->tx_buf.  Returns its length, or 0 if
 * nothing is ready / the chip has no room.
 */
static size_t agg_build(struct ssv_dev *sd, struct ssv_sta *ss, struct ssv_agg *a,
			int hwq)
{
	struct ssv_tx_desc *d = (struct ssv_tx_desc *)sd->tx_buf;
	u8 chain[SSV_TX_MAX_RATES];
	struct sk_buff_head *src;
	struct sk_buff *skb, *first_skb = NULL;
	size_t len = SSV_TX_DESC_LEN, max_len;
	u16 start;
	u8 id;
	int n = 0, limit, i;
	u8 *p;

	if (agg_inflight_count(a) >= AGG_MAX_INFLIGHT)
		return 0;
	if (!ssv_rc_agg_chain(sd, ss, chain))
		return 0;
	max_len = min_t(size_t, agg_max_len[chain[SSV_TX_MAX_RATES - 1] - SSV_RATE_MCS_LGI],
			AGG_MAX_BYTES);
	max_len = min_t(size_t, max_len, SSV_TX_BUF_SIZE);
	limit = min_t(int, a->buf_size, AGG_MAX_FRAMES);
	id = a->next_id++;

	/* everything sent must stay inside the peer's window */
	if (!agg_window_start(a, &start)) {
		skb = skb_peek(&a->q);
		if (!skb)
			return 0;
		start = skb_seq(skb);
	}

	/* retries first (they are older), then new frames */
	p = sd->tx_buf + SSV_TX_DESC_LEN;
	for (src = &a->retry; ; src = &a->q) {
		while (n < limit && (skb = skb_peek(src))) {
			struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
			size_t sz = agg_mpdu_size(skb);
			struct ssv_agg_cb *cb;
			u16 dl;

			if (((skb_seq(skb) - start) & SEQ_MASK) >= a->buf_size ||
			    len + sz > max_len)
				break;
			__skb_unlink(skb, src);
			if (src == &a->q)
				atomic_dec(&sd->agg_queued);

			hdr->duration_id = cpu_to_le16(AGG_MPDU_NAV);
			dl = skb->len + AGG_FCS_LEN;
			put_unaligned_le16(dl << 4, p);
			p[2] = delim_crc(p);
			p[3] = AGG_SIGNATURE;
			memcpy(p + AGG_DELIM_LEN, skb->data, skb->len);
			memset(p + AGG_DELIM_LEN + skb->len, 0,
			       sz - AGG_DELIM_LEN - skb->len);
			p += sz;
			len += sz;
			n++;

			cb = agg_cb(skb);
			cb->sent_at = jiffies;
			cb->rate = chain[0];
			cb->id = id;
			__skb_queue_tail(&a->inflight, skb);
			if (!first_skb)
				first_skb = skb;
		}
		if (src == &a->q)
			break;
	}
	if (!n)
		return 0;

	memset(d, 0, SSV_TX_DESC_LEN);
	d->len = len;
	d->c_type = M2_TXREQ;
	d->f80211 = 1;
	d->qos = 1;
	d->unicast = 1;
	d->wsid = ss->wsid;
	d->txq_idx = hwq;
	d->hdr_offset = TXPB_OFFSET;
	d->hdr_len = ieee80211_hdrlen(((struct ieee80211_hdr *)first_skb->data)->frame_control);
	d->payload_offset = TXPB_OFFSET + d->hdr_len;
	d->ack_policy = 1;
	d->aggregation = 1;
	d->RSVD_1 = 1;
	d->do_rts_cts = 1;
	d->tx_report = 1;
	d->fCmd = ((hwq + M_ENG_TX_EDCA0) << 4) | M_ENG_HWHCI;
	for (i = 0; i < SSV_TX_MAX_RATES; i++) {
		d->rc_params[i].count = 2;
		agg_set_timing(d, &d->rc_params[i], chain[i], len + AGG_FCS_LEN);
	}
	d->drate_idx = d->rc_params[0].drate;
	d->crate_idx = d->rc_params[0].crate;
	d->rts_cts_nav = d->rc_params[0].rts_cts_nav;
	d->frame_consume_time = d->rc_params[0].frame_consume_time;
	d->dl_length = d->rc_params[0].dl_length;

	dev_dbg(sd->dev, "agg: q%d id %u send %d mpdu seq %u len %zu rate %u\n",
		hwq, id, n, skb_seq(first_skb), len, chain[0]);
	return len;
}

/* Tell the peer to move its window past MPDUs we gave up on. */
static void agg_send_bar(struct ssv_dev *sd, struct ieee80211_sta *sta,
			 struct ssv_agg *a, u8 tid)
{
	struct sk_buff *skb;
	u16 start;

	if (!sd->vif)
		return;
	if (!agg_window_start(a, &start)) {
		skb = skb_peek(&a->q);
		if (!skb)
			return;
		start = skb_seq(skb);
	}
	ieee80211_send_bar(sd->vif, sta->addr, tid, start);
}

/*
 * TX thread: send pending aggregates.  Returns true if something was sent,
 * *blocked if the chip had no room.
 */
bool ssv_agg_pump(struct ssv_dev *sd, bool *blocked)
{
	struct sk_buff_head drop;
	bool sent = false;
	int w, t;

	__skb_queue_head_init(&drop);
	for (w = 0; w < SSV_NUM_STA; w++) {
		struct ieee80211_sta *sta;
		struct ssv_sta *ss;

		/* aggregates are written to the bus, which sleeps: no RCU here */
		mutex_lock(&sd->agg_mutex);
		sta = rcu_dereference_protected(sd->sta[w],
						lockdep_is_held(&sd->agg_mutex));
		if (!sta) {
			mutex_unlock(&sd->agg_mutex);
			continue;
		}
		ss = (struct ssv_sta *)sta->drv_priv;
		for (t = 0; t < SSV_AGG_TIDS; t++) {
			struct ssv_agg *a = &ss->agg[t];
			int hwq = ssv_tid_to_hwq(t);
			struct sk_buff *skb, *next;
			bool gave_up = false;
			size_t len;

			if (a->state != SSV_AGG_OPERATIONAL)
				continue;

			/* aggregates with neither a Block Ack nor a NO_BA */
			spin_lock_bh(&sd->sta_lock);
			skb_queue_walk_safe(&a->inflight, skb, next) {
				if ((u32)jiffies - agg_cb(skb)->sent_at < AGG_BA_TIMEOUT)
					continue;
				dev_dbg(sd->dev, "agg: tid %d seq %u timed out\n",
					t, skb_seq(skb));
				__skb_unlink(skb, &a->inflight);
				gave_up |= agg_retry(a, skb, &drop);
			}
			spin_unlock_bh(&sd->sta_lock);
			if (gave_up)
				agg_send_bar(sd, sta, a, t);

			while (!skb_queue_empty(&a->retry) || !skb_queue_empty(&a->q)) {
				if (!ssv_tx_budget(sd, hwq, AGG_MAX_BYTES)) {
					*blocked = true;
					break;
				}
				spin_lock_bh(&sd->sta_lock);
				len = agg_build(sd, ss, a, hwq);
				spin_unlock_bh(&sd->sta_lock);
				if (!len)
					break;
				if (ssv_tx_write(sd, hwq, len))
					break;	/* the timeout takes care of them */
				sent = true;
			}
		}
		mutex_unlock(&sd->agg_mutex);
	}
	agg_complete(sd, &drop, false);
	return sent;
}

static struct ssv_agg *agg_lookup(struct ssv_dev *sd, u8 wsid, u8 tid,
				  struct ieee80211_sta **stap)
{
	struct ieee80211_sta *sta;

	if (wsid >= SSV_NUM_STA || tid >= SSV_AGG_TIDS)
		return NULL;
	sta = rcu_dereference(sd->sta[wsid]);
	if (!sta)
		return NULL;
	*stap = sta;
	return &((struct ssv_sta *)sta->drv_priv)->agg[tid];
}

static struct sk_buff *agg_find(struct ssv_agg *a, u16 seq)
{
	struct sk_buff *skb;

	skb_queue_walk(&a->inflight, skb)
		if (skb_seq(skb) == seq)
			return skb;
	return NULL;
}

/*
 * Settle the MPDUs listed in @note: acknowledged if @bitmap (starting at
 * @ssn) has their bit, resent otherwise.  @bitmap NULL means no Block Ack.
 */
static void agg_settle(struct ssv_dev *sd, struct ieee80211_sta *sta,
		       struct ssv_agg *a, u8 tid, const struct ssv_ba_note *note,
		       u16 ssn, const __le32 *bitmap)
{
	struct ssv_sta *ss = (struct ssv_sta *)sta->drv_priv;
	struct sk_buff_head done, drop;
	int i, frames = 0, acked = 0, rate = -1;

	__skb_queue_head_init(&done);
	__skb_queue_head_init(&drop);

	spin_lock_bh(&sd->sta_lock);
	for (i = 0; i < ARRAY_SIZE(note->seq); i++) {
		u16 seq = le16_to_cpu(note->seq[i]);
		struct sk_buff *skb;
		u16 off;

		if (seq > SEQ_MASK)
			break;
		skb = agg_find(a, seq);
		if (!skb)
			continue;
		__skb_unlink(skb, &a->inflight);
		if (rate < 0)
			rate = agg_cb(skb)->rate;
		frames++;
		off = (seq - ssn) & SEQ_MASK;
		if (bitmap && off < 64 &&
		    (le32_to_cpu(bitmap[off / 32]) & BIT(off % 32))) {
			agg_acked(a, skb, &done);
			acked++;
		} else {
			agg_retry(a, skb, &drop);
		}
	}
	if (rate >= 0)
		ssv_rc_agg_result(sd, ss, rate, frames, acked,
				  note->tried[0].count);
	spin_unlock_bh(&sd->sta_lock);

	dev_dbg(sd->dev, "agg: %s tid %u ssn %u acked %d/%d tried %u\n",
		bitmap ? "BA" : "NO_BA", tid, ssn, acked, frames,
		note->tried[0].count);
	if (!skb_queue_empty(&drop))
		agg_send_bar(sd, sta, a, tid);
	ssv_tx_kick(sd);
	agg_complete(sd, &done, true);
	agg_complete(sd, &drop, false);
}

/* Block Ack forwarded by the firmware (RX path, process context). */
void ssv_agg_ba(struct ssv_dev *sd, struct sk_buff *skb)
{
	const struct ssv_ba_frame *ba = (const struct ssv_ba_frame *)skb->data;
	const struct ssv_ba_note *note;
	struct ieee80211_sta *sta;
	struct ssv_agg *a;

	if (skb->len < sizeof(*ba) + sizeof(*note))
		return;
	note = (const struct ssv_ba_note *)(skb->data + skb->len - sizeof(*note));

	rcu_read_lock();
	a = agg_lookup(sd, note->wsid, le16_to_cpu(ba->control) >> 12, &sta);
	if (a && a->state == SSV_AGG_OPERATIONAL)
		agg_settle(sd, sta, a, le16_to_cpu(ba->control) >> 12, note,
			   le16_to_cpu(ba->ssc) >> 4, ba->bitmap);
	rcu_read_unlock();
}

/* The firmware gave up on an aggregate without any Block Ack. */
void ssv_agg_no_ba(struct ssv_dev *sd, const u8 *data, size_t len)
{
	const struct ssv_ba_note *note = (const struct ssv_ba_note *)data;
	const struct ieee80211_hdr *hdr;
	struct ieee80211_sta *sta;
	struct ssv_agg *a;
	u8 tid;

	if (len < sizeof(*note) + 26)
		return;
	hdr = (const struct ieee80211_hdr *)(note + 1);
	if (!ieee80211_is_data_qos(hdr->frame_control))
		return;
	tid = *ieee80211_get_qos_ctl((struct ieee80211_hdr *)hdr) &
	      IEEE80211_QOS_CTL_TID_MASK;

	rcu_read_lock();
	a = agg_lookup(sd, note->wsid, tid, &sta);
	if (a && a->state == SSV_AGG_OPERATIONAL)
		agg_settle(sd, sta, a, tid, note, 0, NULL);
	rcu_read_unlock();
}

int ssv_agg_action(struct ssv_dev *sd, struct ieee80211_vif *vif,
		   struct ieee80211_ampdu_params *params)
{
	struct ssv_sta *ss = (struct ssv_sta *)params->sta->drv_priv;
	u8 tid = params->tid;
	struct ssv_agg *a;

	if (tid >= SSV_AGG_TIDS)
		return -EINVAL;
	a = &ss->agg[tid];
	dev_dbg(sd->dev, "agg: action %d tid %u buf %u\n", params->action, tid,
		params->buf_size);

	switch (params->action) {
	case IEEE80211_AMPDU_TX_START:
		spin_lock_bh(&sd->sta_lock);
		a->state = SSV_AGG_STARTING;
		spin_unlock_bh(&sd->sta_lock);
		return IEEE80211_AMPDU_TX_START_IMMEDIATE;
	case IEEE80211_AMPDU_TX_OPERATIONAL:
		spin_lock_bh(&sd->sta_lock);
		a->buf_size = clamp_t(u16, params->buf_size, 1, SSV_AGG_WINDOW);
		memset(a->tries, 0, sizeof(a->tries));
		a->state = SSV_AGG_OPERATIONAL;
		spin_unlock_bh(&sd->sta_lock);
		return 0;
	case IEEE80211_AMPDU_TX_STOP_CONT:
		ssv_agg_flush(sd, ss, tid);
		ieee80211_stop_tx_ba_cb_irqsafe(vif, params->sta->addr, tid);
		return 0;
	case IEEE80211_AMPDU_TX_STOP_FLUSH:
	case IEEE80211_AMPDU_TX_STOP_FLUSH_CONT:
		ssv_agg_flush(sd, ss, tid);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}
