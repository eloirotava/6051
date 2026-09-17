// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SSV6051 transmit path.
 *
 * mac80211 hands frames to ssv_tx(), which only queues them.  A kernel
 * thread builds the chip descriptor, checks the chip-side buffer budget
 * and writes the frame over SDIO.  The chip retries on its own and does
 * not report per-frame status, so frames are completed as acknowledged
 * right after the write (like the vendor driver).
 */
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/freezer.h>
#include <linux/kthread.h>

#include "ssv6051.h"

#define TXQ_STOP_LEN		64
#define TXQ_WAKE_LEN		32

/* per-queue frame budget in the chip: BK, BE, VI, VO, management */
static const u8 hwq_max_frames[HW_TXQ_NUM] = { 8, 15, 16, 16, 8 };
static const u8 ac_to_hwq[IEEE80211_NUM_ACS] = { 3, 2, 1, 0 };

/* Airtime constants, microseconds */
#define CCK_SIFS		10
#define CCK_PREAMBLE_BITS	144
#define CCK_PLCP_BITS		48
#define OFDM_SIFS		16
#define OFDM_PREAMBLE		20
#define OFDM_PLCP_BITS		22
#define OFDM_SYMBOL		4
#define HT_SIFS			10
#define HT_PREAMBLE		(8 + 8 + 4 + 8 + 4 + 4 + 6)	/* L-STF..HT-LTF + ext */
#define ACK_LEN			14
#define RTS_LEN			20
#define CTS_LEN			14

static const u16 ht_bits_per_symbol[8] = { 26, 52, 78, 104, 156, 208, 234, 260 };

u32 ssv_legacy_airtime(const struct ssv_rate *r, u32 len, bool short_pre)
{
	u32 bits = len * 8;

	if (r->phy == SSV_PHY_CCK) {
		u32 pre = CCK_PREAMBLE_BITS + CCK_PLCP_BITS;

		if (short_pre)
			pre >>= 1;
		return CCK_SIFS + pre + bits * 1000 / r->kbps;
	}
	return OFDM_SIFS + OFDM_PREAMBLE +
	       DIV_ROUND_UP(OFDM_PLCP_BITS + bits, r->kbps * OFDM_SYMBOL / 1000) *
	       OFDM_SYMBOL;
}

u32 ssv_ht_airtime(u8 mcs, u32 len, bool sgi)
{
	u32 nsym = DIV_ROUND_UP(len * 8 + OFDM_PLCP_BITS, ht_bits_per_symbol[mcs & 7]);
	u32 t = sgi ? DIV_ROUND_UP((nsym * 18 + 4) / 5, 4) << 2 : nsym << 2;

	return t + HT_PREAMBLE + HT_SIFS;
}

/*
 * Fill the NAV/duration fields the chip needs for this rate.  Returns the
 * ACK duration for the 802.11 Duration/ID field.
 */
static u32 ssv_set_timing(struct ssv_dev *sd, struct ssv_tx_desc *d,
			  u8 drate, u32 len, bool rts)
{
	const struct ssv_rate *r = &ssv_rates[drate];
	const struct ssv_rate *c = &ssv_rates[r->ctrl];
	bool short_pre = sd->short_preamble;
	u32 frame, ack = 0, nav = 0, consume = 0;

	if (r->phy == SSV_PHY_HT)
		frame = ssv_ht_airtime(r->dot11, len, drate >= SSV_RATE_MCS_SGI);
	else
		frame = ssv_legacy_airtime(r, len, short_pre);

	if (d->unicast)
		ack = ssv_legacy_airtime(c, ACK_LEN, short_pre);
	if (rts) {
		nav = frame + ack + ssv_legacy_airtime(c, CTS_LEN, short_pre);
		consume = nav + ssv_legacy_airtime(c, RTS_LEN, short_pre);
	}

	d->rts_cts_nav = nav;
	d->frame_consume_time = (consume >> 5) + 1;
	if (r->phy == SSV_PHY_HT) {
		u32 l = frame - HT_SIFS;

		/* legacy L-SIG length spoofing the HT PPDU duration */
		l = ((l - (6 + 20)) + 3) >> 2;
		d->dl_length = l + (l << 1) - 3;
	}
	return ack;
}

static int ssv_hdrlen(struct ieee80211_hdr *hdr)
{
	return ieee80211_hdrlen(hdr->frame_control);
}

static u8 ssv_sband_to_rate(struct ssv_dev *sd, int i)
{
	return i < 4 ? i : SSV_RATE_OFDM + (i - 4);
}

/* Lowest basic rate of the BSS (1 Mbps before association). */
static u8 ssv_low_rate(struct ssv_dev *sd)
{
	struct ieee80211_vif *vif = sd->vif;
	u32 basic = 0;

	if (vif && (vif->cfg.assoc || vif->type == NL80211_IFTYPE_AP))
		basic = vif->bss_conf.basic_rates;

	return basic ? ssv_sband_to_rate(sd, __ffs(basic)) : 0;
}

static u8 ssv_cck_preamble(struct ssv_dev *sd, u8 rate)
{
	if (sd->short_preamble && rate >= 1 && rate <= 3)
		return rate + 3;
	return rate;
}

static bool ssv_build_desc(struct ssv_dev *sd, struct sk_buff *skb,
			   struct ieee80211_sta *sta, int hwq)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	struct ssv_tx_desc *d;
	bool report = false, rts;
	u32 ack, len;
	int hdrlen = ssv_hdrlen(hdr);
	u8 rate, wsid = 0x0f;
	__le16 fc = hdr->frame_control;

	if (skb_headroom(skb) < SSV_TX_DESC_LEN ||
	    skb->len + SSV_TX_DESC_LEN > SSV_MAX_FRAME)
		return false;

	if (sta && ((struct ssv_sta *)sta->drv_priv)->wsid < 0)
		sta = NULL;
	if (sta)
		wsid = ((struct ssv_sta *)sta->drv_priv)->wsid;
	if (sta && ieee80211_is_data(fc) && skb->protocol != cpu_to_be16(ETH_P_PAE))
		rate = ssv_rc_get(sd, (struct ssv_sta *)sta->drv_priv, &report);
	else
		rate = ssv_low_rate(sd);
	rate = ssv_cck_preamble(sd, rate);

	len = skb->len + FCS_LEN;
	rts = sd->hw->wiphy->rts_threshold != (u32)-1 &&
	      len > sd->hw->wiphy->rts_threshold;
	if (!rts && sd->vif && sd->vif->bss_conf.use_cts_prot &&
	    ssv_rates[rate].phy != SSV_PHY_CCK && ieee80211_is_data(fc))
		rts = true;	/* the chip only does RTS/CTS, not CTS-to-self */

	d = skb_push(skb, SSV_TX_DESC_LEN);
	memset(d, 0, SSV_TX_DESC_LEN);
	d->len = skb->len;
	d->c_type = M2_TXREQ;
	d->f80211 = 1;
	d->qos = ieee80211_is_data_qos(fc);
	d->use_4addr = ieee80211_has_a4(fc);
	d->more_data = ieee80211_has_morefrags(fc);
	d->stype_b5b4 = (le16_to_cpu(fc) >> 4) & 0x3;
	d->frag = d->more_data || (le16_to_cpu(hdr->seq_ctrl) & 0xf);
	d->unicast = !is_multicast_ether_addr(hdr->addr1);
	d->tx_burst = d->frag;
	d->wsid = wsid;
	d->txq_idx = hwq;
	d->hdr_offset = TXPB_OFFSET;
	d->hdr_len = hdrlen;
	d->payload_offset = TXPB_OFFSET + hdrlen;
	d->do_rts_cts = rts ? 1 : 0;
	d->drate_idx = rate;
	d->crate_idx = ssv_rates[rate].ctrl;

	if (!d->unicast || (info->flags & IEEE80211_TX_CTL_NO_ACK) ||
	    ieee80211_is_ctl(fc))
		d->ack_policy = 1;
	else if (d->qos)
		d->ack_policy = (*ieee80211_get_qos_ctl(hdr) & 0x60) >> 5;

	d->fCmd = ((hwq + M_ENG_TX_EDCA0) << 4) | M_ENG_HWHCI;
	if (report) {
		d->RSVD_0 = SSV_TXREPORT_RC;
		d->tx_report = 1;
	}

	ack = ssv_set_timing(sd, d, rate, len, rts);
	if (!d->tx_burst && d->ack_policy != 1)
		hdr->duration_id = cpu_to_le16(ack);
	return true;
}

static void ssv_tx_complete(struct ssv_dev *sd, struct sk_buff *skb, bool ok)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);

	skb_pull(skb, SSV_TX_DESC_LEN);
	ieee80211_tx_info_clear_status(info);
	if (ok && !(info->flags & IEEE80211_TX_CTL_NO_ACK))
		info->flags |= IEEE80211_TX_STAT_ACK;
	ieee80211_tx_status_ni(sd->hw, skb);
}

/* Read how much of the chip TX buffer is in use. */
static int ssv_refresh_resources(struct ssv_dev *sd)
{
	u32 info, info2;
	int ret;

	ret = ssv_reg_read(sd, ADR_TX_ID_ALL_INFO, &info);
	if (!ret)
		ret = ssv_reg_read(sd, ADR_TX_ID_ALL_INFO2, &info2);
	if (ret) {
		sd->res_valid = false;
		return ret;
	}
	/* INFO: page:8 id:6 q0:4 q1:4 q2:5 q3:5; INFO2: page:9 id:8 q4:4 */
	sd->free_pages = HW_TX_PAGES - (int)(info2 & 0x1ff);
	sd->free_ids = HW_TX_IDS - (int)((info2 >> 9) & 0xff);
	sd->free_frames[0] = hwq_max_frames[0] - (int)((info >> 14) & 0xf);
	sd->free_frames[1] = hwq_max_frames[1] - (int)((info >> 18) & 0xf);
	sd->free_frames[2] = hwq_max_frames[2] - (int)((info >> 22) & 0x1f);
	sd->free_frames[3] = hwq_max_frames[3] - (int)((info >> 27) & 0x1f);
	sd->free_frames[4] = hwq_max_frames[4] - (int)((info2 >> 17) & 0xf);
	sd->res_valid = true;
	return 0;
}

static int ssv_len_pages(size_t len)
{
	return DIV_ROUND_UP(len + TX_ALLOC_RSVD, 1 << HW_PAGE_SHIFT);
}

static bool ssv_can_send_len(struct ssv_dev *sd, int hwq, size_t len)
{
	return sd->res_valid && sd->free_pages >= ssv_len_pages(len) &&
	       sd->free_ids > 0 && sd->free_frames[hwq] > 0;
}

static bool ssv_can_send(struct ssv_dev *sd, int hwq, struct sk_buff *skb)
{
	return ssv_can_send_len(sd, hwq, skb->len);
}

/* Is there chip room for up to @len bytes on @hwq?  Refreshes once. */
bool ssv_tx_budget(struct ssv_dev *sd, int hwq, size_t len)
{
	if (ssv_can_send_len(sd, hwq, len))
		return true;
	ssv_refresh_resources(sd);
	return ssv_can_send_len(sd, hwq, len);
}

/* Write @len bytes prepared in sd->tx_buf and charge the chip budget. */
int ssv_tx_write(struct ssv_dev *sd, int hwq, size_t len)
{
	size_t aligned = sdio_align_size(sd->func, len);
	int ret;

	sd->free_pages -= ssv_len_pages(len);
	sd->free_ids--;
	sd->free_frames[hwq]--;
	memset(sd->tx_buf + len, 0, aligned - len);
	ret = ssv_write_data(sd, sd->tx_buf, len);
	if (ret)
		sd->res_valid = false;
	return ret;
}

static void ssv_send_one(struct ssv_dev *sd, int hwq, struct sk_buff *skb)
{
	int ret;

	/* bounce: word-aligned, DMA-safe and zero-padded to the block size */
	memcpy(sd->tx_buf, skb->data, skb->len);
	ret = ssv_tx_write(sd, hwq, skb->len);
	ssv_tx_complete(sd, skb, !ret);
}

int ssv_tid_to_hwq(u8 tid)
{
	static const u8 tid_to_ac[8] = {
		IEEE80211_AC_BE, IEEE80211_AC_BK, IEEE80211_AC_BK, IEEE80211_AC_BE,
		IEEE80211_AC_VI, IEEE80211_AC_VI, IEEE80211_AC_VO, IEEE80211_AC_VO,
	};

	return ac_to_hwq[tid_to_ac[tid & 7]];
}

static unsigned int ssv_queued_hw(struct ssv_dev *sd)
{
	unsigned int n = 0;
	int q;

	for (q = 0; q < HW_TXQ_NUM; q++)
		n += skb_queue_len(&sd->txq[q]);
	return n;
}

/* everything not yet handed to the chip, for flow control */
static unsigned int ssv_queued(struct ssv_dev *sd)
{
	return ssv_queued_hw(sd) + atomic_read(&sd->agg_queued);
}

/* an aggregation queue has work (new frames, or a Block Ack came in) */
void ssv_tx_kick(struct ssv_dev *sd)
{
	WRITE_ONCE(sd->agg_kick, true);
	wake_up(&sd->tx_wait);
}

static int ssv_tx_thread(void *data)
{
	struct ssv_dev *sd = data;

	set_freezable();
	while (!kthread_should_stop()) {
		bool sent = false, blocked = false;
		int q;

		/* aggregates waiting for a Block Ack need a periodic look */
		wait_event_freezable_timeout(sd->tx_wait,
						 ssv_queued_hw(sd) || READ_ONCE(sd->agg_kick) ||
						 kthread_should_stop(),
						 msecs_to_jiffies(50));
		if (kthread_should_stop())
			break;
		WRITE_ONCE(sd->agg_kick, false);

		if (ssv_agg_pump(sd, &blocked))
			sent = true;

		/* management first, then VO, VI, BE, BK */
		for (q = HW_TXQ_NUM - 1; q >= 0; q--) {
			struct sk_buff *skb;

			while ((skb = skb_peek(&sd->txq[q]))) {
				if (!ssv_can_send(sd, q, skb)) {
					if (!sent && !blocked)
						ssv_refresh_resources(sd);
					if (!ssv_can_send(sd, q, skb)) {
						blocked = true;
						break;
					}
				}
				skb_unlink(skb, &sd->txq[q]);
				ssv_send_one(sd, q, skb);
				sent = true;
			}
		}

		if (sd->queues_stopped && ssv_queued(sd) < TXQ_WAKE_LEN) {
			sd->queues_stopped = false;
			ieee80211_wake_queues(sd->hw);
		}

		/*
		 * Chip buffer full: let the air drain it.  Re-reading the
		 * budget in a tight loop only starves the bus.
		 */
		if (blocked && !sent) {
			sd->res_valid = false;
			usleep_range(500, 1000);
		}
	}
	return 0;
}

void ssv_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control,
	    struct sk_buff *skb)
{
	struct ssv_dev *sd = hw->priv;
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	struct ieee80211_sta *sta = control ? control->sta : NULL;
	__le16 fc = hdr->frame_control;
	bool ap = ssv_is_ap(sd);
	int hwq;

	if (!sd->started) {
		ieee80211_free_txskb(hw, skb);
		return;
	}

	/*
	 * Chip queue 4 carries management frames for a station; an AP uses
	 * it for group frames held until the DTIM beacon.
	 */
	if (ap && (info->flags & IEEE80211_TX_CTL_SEND_AFTER_DTIM))
		hwq = HW_TXQ_MGMT;
	else if (!ap && (ieee80211_is_mgmt(fc) || ieee80211_is_any_nullfunc(fc)))
		hwq = HW_TXQ_MGMT;
	else if (skb->protocol == cpu_to_be16(ETH_P_PAE))
		hwq = ac_to_hwq[IEEE80211_AC_VO];
	else
		hwq = ac_to_hwq[skb_get_queue_mapping(skb) & 3];

	if (sta && ssv_agg_tx(sd, sta, skb)) {
		ssv_tx_kick(sd);
	} else {
		info->flags &= ~IEEE80211_TX_CTL_AMPDU;
		if (ap && hwq == HW_TXQ_MGMT)
			ssv_ap_group_queued(sd);
		if (!ssv_build_desc(sd, skb, sta, hwq)) {
			ieee80211_free_txskb(hw, skb);
			return;
		}
		skb_queue_tail(&sd->txq[hwq], skb);
		wake_up(&sd->tx_wait);
	}

	if (!sd->queues_stopped && ssv_queued(sd) >= TXQ_STOP_LEN) {
		sd->queues_stopped = true;
		ieee80211_stop_queues(hw);
	}
}

void ssv_tx_flush(struct ssv_dev *sd)
{
	struct sk_buff *skb;
	int q;

	for (q = 0; q < HW_TXQ_NUM; q++)
		while ((skb = skb_dequeue(&sd->txq[q])))
			ssv_tx_complete(sd, skb, false);
}

int ssv_tx_init(struct ssv_dev *sd)
{
	int q;

	for (q = 0; q < HW_TXQ_NUM; q++)
		skb_queue_head_init(&sd->txq[q]);
	init_waitqueue_head(&sd->tx_wait);
	atomic_set(&sd->agg_queued, 0);
	sd->tx_buf = devm_kmalloc(sd->dev, SSV_TX_BUF_SIZE + SDIO_BLOCK_SIZE, GFP_KERNEL);
	if (!sd->tx_buf)
		return -ENOMEM;
	sd->res_valid = false;
	sd->tx_thread = kthread_run(ssv_tx_thread, sd, "ssv6051-tx");
	if (IS_ERR(sd->tx_thread)) {
		int ret = PTR_ERR(sd->tx_thread);

		sd->tx_thread = NULL;
		return ret;
	}
	return 0;
}

void ssv_tx_deinit(struct ssv_dev *sd)
{
	if (sd->tx_thread) {
		kthread_stop(sd->tx_thread);
		sd->tx_thread = NULL;
	}
	ssv_tx_flush(sd);
}
