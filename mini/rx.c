// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SSV6051 receive path, run from the SDIO interrupt work.
 */
#include <linux/unaligned.h>

#include "ssv6051.h"

#define RX_BUDGET	32

static int ssv_read_status(struct ssv_dev *sd, u8 *status)
{
	int ret;

	sdio_claim_host(sd->func);
	*status = sdio_readb(sd->func, SDIO_REG_INT_STATUS, &ret);
	sdio_release_host(sd->func);
	return ret;
}

/*
 * The frame length lives in two function-1 registers.  The chip does not
 * accept CMD53 on them, so it takes two CMD52s.
 */
static struct sk_buff *ssv_read_frame(struct ssv_dev *sd)
{
	struct sdio_func *func = sd->func;
	struct sk_buff *skb = NULL;
	size_t aligned;
	u32 len;
	int ret;

	sdio_claim_host(func);
	len = sdio_readb(func, SDIO_REG_RX_LEN0, &ret);
	if (!ret)
		len |= sdio_readb(func, SDIO_REG_RX_LEN1, &ret) << 8;
	if (ret)
		goto out;
	aligned = sdio_align_size(func, len);
	if (len < sizeof(struct ssv_host_event) || aligned > SSV_MAX_FRAME) {
		dev_err_ratelimited(sd->dev, "bogus RX length %u\n", len);
		goto out;
	}
	skb = dev_alloc_skb(aligned);
	if (!skb)
		goto out;
	ret = sdio_memcpy_fromio(func, skb->data, sd->data_port, aligned);
	if (ret) {
		dev_err_ratelimited(sd->dev, "RX read failed: %d\n", ret);
		dev_kfree_skb(skb);
		skb = NULL;
		goto out;
	}
	skb_put(skb, len);
out:
	sdio_release_host(func);
	return skb;
}

static void ssv_rx_event(struct ssv_dev *sd, struct sk_buff *skb)
{
	struct ssv_host_event *ev = (struct ssv_host_event *)skb->data;

	switch (ev->h_event) {
	case SSV_EVT_TXLOOPBK_RESULT:
		sd->cali_state = ev->seq_no == 0 ? 1 : -1;
		wake_up(&sd->cali_wait);
		break;
	case SSV_EVT_NO_BA:
		ssv_agg_no_ba(sd, ev->data, skb->len - sizeof(*ev));
		break;
	case SSV_EVT_RC_MPDU_REPORT:
		if (skb->len >= sizeof(*ev) + sizeof(struct ssv_rc_report))
			ssv_rc_report(sd, (struct ssv_rc_report *)ev->data);
		break;
	default:
		/* watchdog ticks, AMPDU/BA notifications, logs */
		dev_dbg(sd->dev, "event %u len %u\n", ev->h_event, skb->len);
		break;
	}
	dev_kfree_skb(skb);
}

static void ssv_rx_rate(struct ieee80211_rx_status *rxs, unsigned int rate)
{
	const struct ssv_rate *r;

	if (rate >= SSV_NUM_RATES)
		rate = 0;
	r = &ssv_rates[rate];
	if (r->phy == SSV_PHY_HT) {
		rxs->encoding = RX_ENC_HT;
		if (rate >= SSV_RATE_MCS_SGI)
			rxs->enc_flags |= RX_ENC_FLAG_SHORT_GI;
	} else {
		rxs->encoding = RX_ENC_LEGACY;
		if (rate >= SSV_RATE_CCK_SHORT && rate < SSV_RATE_OFDM)
			rxs->enc_flags |= RX_ENC_FLAG_SHORTPRE;
	}
	rxs->rate_idx = r->dot11;
}

static void ssv_rx_frame(struct ssv_dev *sd, struct sk_buff *skb)
{
	struct ssv_rx_desc *rxd = (struct ssv_rx_desc *)skb->data;
	struct ssv_rxphy_info *phy = (struct ssv_rxphy_info *)(rxd + 1);
	struct ieee80211_rx_status *rxs = IEEE80211_SKB_RXCB(skb);
	struct ieee80211_hdr *hdr;
	int rpci;

	if (skb->len < SSV_RX_DESC_LEN + RX_PINFO_PAD + 10) {
		dev_kfree_skb(skb);
		return;
	}

	memset(rxs, 0, sizeof(*rxs));
	ssv_rx_rate(rxs, rxd->rate_idx);
	rxs->band = NL80211_BAND_2GHZ;
	rxs->freq = ieee80211_channel_to_frequency(sd->channel, NL80211_BAND_2GHZ);

	/* CCK frames carry the PHY info in the trailing 4 bytes */
	if (rxd->rate_idx < SSV_RATE_OFDM) {
		struct ssv_rxphy_pad *pad = (struct ssv_rxphy_pad *)
			(skb->data + skb->len - sizeof(*pad));

		rpci = pad->rpci;
	} else {
		rpci = phy->rpci;
	}
	rxs->signal = -min(rpci, 88);
	if (phy->aggregate)
		rxs->flag |= RX_FLAG_NO_SIGNAL_VAL;

	skb_pull(skb, SSV_RX_DESC_LEN);
	hdr = (struct ieee80211_hdr *)skb->data;
	/* Block Acks for our aggregates end with the firmware's note */
	if (ieee80211_is_back(hdr->frame_control)) {
		ssv_agg_ba(sd, skb);
		dev_kfree_skb(skb);
		return;
	}
	skb_trim(skb, skb->len - RX_PINFO_PAD);
	/* the chip clock is not the TSF; keep mac80211's beacon timing sane */
	if (ieee80211_is_beacon(hdr->frame_control) ||
	    ieee80211_is_probe_resp(hdr->frame_control)) {
		struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)hdr;

		if (skb->len >= offsetofend(struct ieee80211_mgmt, u.beacon.timestamp))
			mgmt->u.beacon.timestamp = cpu_to_le64(ktime_to_us(ktime_get_boottime()));
	}

	ieee80211_rx_irqsafe(sd->hw, skb);
}

void ssv_rx_irq(struct ssv_dev *sd)
{
	u8 status;
	int n = 0;

	while (n < RX_BUDGET) {
		struct sk_buff *skb;

		if (ssv_read_status(sd, &status) || !(status & SSV_INT_RX))
			break;
		skb = ssv_read_frame(sd);
		if (!skb)
			break;
		n++;
		if (((struct ssv_rx_desc *)skb->data)->c_type == HOST_EVENT)
			ssv_rx_event(sd, skb);
		else
			ssv_rx_frame(sd, skb);
	}
}
