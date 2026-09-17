/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * mac80211 driver for the South Silicon Valley SSV6051 SDIO 802.11b/g/n
 * chip (station and access point, software crypto).
 *
 * Hardware interface derived from the iComm vendor driver:
 * Copyright (c) 2015 South Silicon Valley Microelectronics Inc.
 * Copyright (c) 2015 iComm Corporation
 */
#ifndef SSV6051_H
#define SSV6051_H

#include <linux/bitfield.h>
#include <linux/etherdevice.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/skbuff.h>
#include <linux/wait.h>
#include <linux/mmc/sdio_func.h>
#include <net/mac80211.h>

#include "reg.h"

#define SSV_FIRMWARE		"ssv/ssv6051-sw.bin"

/* SDIO function 1 registers (CMD52) */
#define SDIO_REG_DATA_PORT0	0x00
#define SDIO_REG_DATA_PORT1	0x01
#define SDIO_REG_DATA_PORT2	0x02
#define SDIO_REG_INT_MASK	0x04
#define SDIO_REG_INT_STATUS	0x08
#define SDIO_REG_FN1_STATUS	0x0c
#define SDIO_REG_RX_LEN0	0x10
#define SDIO_REG_RX_LEN1	0x11
#define SDIO_REG_OUTPUT_TIMING	0x55
#define SDIO_REG_PMU_WAKEUP	0x67
#define SDIO_REG_REG_PORT0	0x70
#define SDIO_REG_REG_PORT1	0x71
#define SDIO_REG_REG_PORT2	0x72

#define SDIO_BLOCK_SIZE		128
#define SDIO_OUTPUT_TIMING	3
#define SDIO_CLOCK_INIT		25000000U
#define SSV_MAX_FRAME		4096
#define SSV_TX_BUF_SIZE		16384

/* INT_STATUS / INT_MASK bits */
#define SSV_INT_RX		BIT(0)

/* Frame/command types in the descriptor c_type field */
#define M0_RXEVENT		3
#define M2_TXREQ		2
#define HOST_CMD		5
#define HOST_EVENT		6

/* Packet engines (descriptor fCmd / RX flow registers) */
#define M_ENG_CPU		0x00
#define M_ENG_HWHCI		0x01
#define M_ENG_MACRX		0x04
#define M_ENG_TX_EDCA0		0x06
#define M_ENG_ENCRYPT_SEC	0x0B
#define M_ENG_TRASH_CAN		0x0F

#define TXPB_OFFSET		80
#define RXPB_OFFSET		80
#define TX_PKT_RSVD_SETTING	3
#define TX_ALLOC_RSVD		(TXPB_OFFSET + TX_PKT_RSVD_SETTING * 16)
#define RX_PINFO_PAD		4

/* Chip-side TX resources (pages of 256 bytes, frame IDs, per-queue frames) */
#define HW_PAGE_SHIFT		8
#define HW_TX_PAGES		115
#define HW_RX_PAGES		115
#define HW_TX_IDS		19
#define HW_RX_IDS		60
#define HW_TXQ_NUM		5
#define HW_TXQ_MGMT		4
#define TX_LOWTHRESHOLD_PAGE	(HW_TX_PAGES - HW_TX_PAGES / 2)
#define TX_LOWTHRESHOLD_ID	2

#define SSV_NUM_HW_STA		2	/* stations the MAC tracks in registers */
#define SSV_NUM_STA		8	/* the rest are watched by the firmware */
#define SSV_NUM_KEY_BUFS	8

#define CHIP_ID_6051Q_P1	0x00000000
#define CHIP_ID_6051Q_P2	0x70000000
#define CHIP_ID_6051Z		0x71000000
#define CHIP_ID_6051Q		0x73000000
#define CHIP_ID_6051P		0x75000000

enum ssv_xtal {
	SSV_XTAL_26M = 0,
	SSV_XTAL_40M,
	SSV_XTAL_24M,
};

enum ssv_cmd_id {
	SSV_CMD_PS = 2,
	SSV_CMD_INIT_CALI = 3,
	SSV_CMD_WATCHDOG_START = 6,
	SSV_CMD_WSID_OP = 8,
};

enum ssv_event {
	SSV_EVT_NO_BA = 1,
	SSV_EVT_RC_MPDU_REPORT = 2,
	SSV_EVT_RC_AMPDU_REPORT = 3,
	SSV_EVT_TXLOOPBK_RESULT = 10,
};

#define SSV_TXREPORT_RC		2	/* TXD_REPORT_TYPE asking for an RC report */

enum ssv_wsid_op {
	SSV_WSID_OP_ADD = 0,
	SSV_WSID_OP_DEL = 1,
	SSV_WSID_OP_PAIRWISE_SET_TYPE = 5,
	SSV_WSID_OP_GROUP_SET_TYPE = 6,
};

#define SSV_WSID_SEC_SW		0

#define SSV_OPMODE_STA		0
#define SSV_OPMODE_AP		1

/*
 * Wire formats: little-endian 32-bit words.  Field masks are named
 * <struct><word>_<field>.
 */
/* TX descriptor, TXPB_OFFSET bytes in front of every frame */
#define TXD0_LEN		GENMASK(15, 0)
#define TXD0_C_TYPE		GENMASK(18, 16)
#define TXD0_F80211		BIT(19)
#define TXD0_QOS		BIT(20)
#define TXD0_USE_4ADDR		BIT(22)
#define TXD0_REPORT_TYPE	GENMASK(25, 23)
#define TXD0_MORE_DATA		BIT(28)
#define TXD0_STYPE_B5B4		GENMASK(30, 29)
#define TXD2_HDR_OFFSET		GENMASK(7, 0)
#define TXD2_FRAG		BIT(8)
#define TXD2_UNICAST		BIT(9)
#define TXD2_HDR_LEN		GENMASK(15, 10)
#define TXD2_TX_REPORT		BIT(16)
#define TXD2_TX_BURST		BIT(17)
#define TXD2_ACK_POLICY		GENMASK(19, 18)
#define TXD2_AGGREGATION	BIT(20)
#define TXD2_AGG_MARK		GENMASK(23, 21)
#define TXD2_RTS_CTS		GENMASK(25, 24)
#define TXD3_PAYLOAD_OFFSET	GENMASK(7, 0)
#define TXD3_WSID		GENMASK(22, 19)
#define TXD3_TXQ_IDX		GENMASK(25, 23)
#define TXD4_RTS_CTS_NAV	GENMASK(15, 0)
#define TXD4_CONSUME_TIME	GENMASK(25, 16)
#define TXD4_CRATE		GENMASK(31, 26)
#define TXD5_DRATE		GENMASK(5, 0)
#define TXD5_DL_LENGTH		GENMASK(17, 6)

#define SSV_TX_MAX_RATES	3

/* One step of the chip's retry chain (aggregates) */
#define RCP0_COUNT		GENMASK(3, 0)
#define RCP0_DRATE		GENMASK(9, 4)
#define RCP0_CRATE		GENMASK(15, 10)
#define RCP0_RTS_CTS_NAV	GENMASK(31, 16)
#define RCP1_CONSUME_TIME	GENMASK(9, 0)
#define RCP1_DL_LENGTH		GENMASK(21, 10)

struct ssv_rc_retry {
	__le32 w0;
	__le32 w1;
};

struct ssv_tx_desc {
	__le32 w0;
	__le32 fcmd;		/* packet engine route */
	__le32 w2;
	__le32 w3;
	__le32 w4;
	__le32 w5;
	__le32 rsvd[8];
	struct ssv_rc_retry rc[SSV_TX_MAX_RATES];
};

/* RX descriptor and PHY info in front of every received frame */
#define RXD0_C_TYPE		GENMASK(18, 16)
#define RXD3_WSID		GENMASK(22, 19)
#define RXD3_RATE_IDX		GENMASK(31, 26)

struct ssv_rx_desc {
	__le32 w0;
	__le32 w1;
	__le32 w2;
	__le32 w3;
};

#define RXPHY1_AGGREGATE	BIT(18)
#define RXPHY4_RPCI		GENMASK(7, 0)

struct ssv_rxphy_info {
	__le32 w0;
	__le32 w1;
	__le32 w2;
	__le32 w3;
	__le32 w4;
};

static_assert(sizeof(struct ssv_tx_desc) == 80);
static_assert(sizeof(struct ssv_rx_desc) + sizeof(struct ssv_rxphy_info) == 36);

#define SSV_TX_DESC_LEN		sizeof(struct ssv_tx_desc)
#define SSV_RX_DESC_LEN		(sizeof(struct ssv_rx_desc) + sizeof(struct ssv_rxphy_info))

/* Host command / firmware event header */
#define HDR0_LEN		GENMASK(15, 0)
#define HDR0_C_TYPE		GENMASK(18, 16)
#define HDR0_ID			GENMASK(31, 24)

struct ssv_host_hdr {
	__le32 w0;
	__le32 seq;
	u8 data[];
};

struct ssv_tx_rate_rpt {
	s8 data_rate;
	u8 count;
} __packed;

struct ssv_rc_report {
	u8 wsid;
	struct ssv_tx_rate_rpt rates[SSV_TX_MAX_RATES];
	__le16 ampdu_len;
	__le16 ampdu_ack_len;
	__le32 ack_signal;
} __packed;

struct ssv_wsid_params {
	u8 cmd;
	u8 wsid_idx;
	u8 target_wsid[ETH_ALEN];
	u8 hw_security;
} __packed;

/* Calibration request, followed by the PHY and RF tables */
struct ssv_iqk_cfg {
	u8 xtal;
	u8 pa;
	u8 pabias_ctrl;
	u8 pacascode_ctrl;
	u8 tssi_trgt;
	u8 tssi_div;
	u8 tx_scale_11b;
	u8 tx_scale_11b_p0d5;
	u8 tx_scale_11g;
	u8 tx_scale_11g_p0d5;
	u8 rsvd[2];
	__le32 cmd_sel;
	__le32 fx_sel;
	__le32 phy_tbl_size;
	__le32 rf_tbl_size;
} __packed;

/*
 * Chip packet-buffer security table: 3 group keys of 48 bytes and 8
 * station entries of 52.  Crypto is done by mac80211; the MAC only needs
 * the (zeroed) table to exist.
 */
#define SSV_HW_SEC_SIZE		(3 * 48 + 8 * 52)

/* Rate table: index == chip rate index */
#define SSV_RATE_CCK_SHORT	4	/* 2/5.5/11 Mbps short preamble: 4..6 */
#define SSV_RATE_OFDM		7	/* 6..54 Mbps: 7..14 */
#define SSV_RATE_MCS_LGI	15	/* MCS0..7: 15..22 */
#define SSV_RATE_MCS_SGI	23	/* MCS0..7 short GI: 23..30 */
#define SSV_NUM_RATES		31

enum ssv_phy {
	SSV_PHY_CCK,
	SSV_PHY_OFDM,
	SSV_PHY_HT,
};

struct ssv_rate {
	u32 kbps;
	u8 phy;
	u8 ctrl;	/* rate index used for ACK/CTS */
	u8 dot11;	/* sband bitrate index or MCS */
};

extern const struct ssv_rate ssv_rates[SSV_NUM_RATES];

/* Rate control state for one peer */
#define SSV_RC_MAX		12
struct ssv_rc {
	u8 rate[SSV_RC_MAX];	/* chip rate indices, ascending speed */
	u8 n;
	u8 cur;
	u32 prob[SSV_RC_MAX];	/* EWMA of ACKs per transmission, 0..1024 */
	bool sampled[SSV_RC_MAX];
	u8 win_idx;		/* rate index used by the current window */
	u8 win_left;
	u32 windows;
	u32 agg_count;
};

#define SSV_AGG_TIDS		8
#define SSV_AGG_WINDOW		64

enum ssv_agg_state {
	SSV_AGG_OFF,
	SSV_AGG_STARTING,
	SSV_AGG_OPERATIONAL,
};

/* TX aggregation state of one TID (protected by ssv_dev.sta_lock) */
struct ssv_agg {
	u8 state;
	u8 next_id;		/* tags the MPDUs of each aggregate */
	u16 buf_size;
	unsigned long retry_start;
	struct sk_buff_head q;
	struct sk_buff_head retry;	/* sorted by sequence number */
	struct sk_buff_head inflight;	/* in send order */
	u8 tries[SSV_AGG_WINDOW];
};

struct ssv_sta {
	int wsid;
	struct ssv_rc rc;
	struct ssv_agg agg[SSV_AGG_TIDS];
};

struct ssv_dev {
	struct sdio_func *func;
	struct device *dev;
	struct ieee80211_hw *hw;
	struct ieee80211_supported_band band;

	/* board configuration */
	u32 xtal;
	bool ldo;
	u32 tx_gain_b;
	u32 tx_gain_gn;
	u32 chip_id;
	u8 mac[ETH_ALEN];

	/* SDIO */
	u32 bus_clock;		/* negotiated by the MMC core */
	u32 data_port;
	u32 reg_port;
	u8 *io_buf;		/* DMA-safe scratch, used under the SDIO host lock */

	/* chip state */
	u32 sec_buf;
	u32 pinfo_buf;
	u32 key_buf[SSV_NUM_KEY_BUFS];
	bool ch13_14;
	int channel;
	bool started;

	/* TX */
	struct sk_buff_head txq[HW_TXQ_NUM];
	struct task_struct *tx_thread;
	wait_queue_head_t tx_wait;
	u8 *tx_buf;
	int free_pages;
	int free_ids;
	int free_frames[HW_TXQ_NUM];
	bool res_valid;
	bool queues_stopped;
	/* frames held in aggregation queues, and a TX thread poke for them */
	atomic_t agg_queued;
	bool agg_kick;

	/* calibration handshake */
	wait_queue_head_t cali_wait;
	int cali_state;

	/* association */
	struct mutex mutex;
	struct mutex agg_mutex;	/* TX thread vs. station removal */
	spinlock_t sta_lock;
	struct ieee80211_vif *vif;

	/* access point: beacon kept by the chip, group frames after DTIM */
	u32 bcn_buf[2];
	u16 bcn_len[2];
	u8 *bcn_last;
	size_t bcn_last_len;
	bool dtim_bit;		/* group frames wait in chip queue 4 */
	struct work_struct beacon_work;
	struct delayed_work dtim_work;
	struct ieee80211_sta __rcu *sta[SSV_NUM_STA];
	bool short_preamble;
	struct ieee80211_sta *rx_ba_sta;	/* owner of the single RX BA session */
	u16 rx_ba_tid;
	u32 cca_control;
	u32 cca_1;
};

/* mac.c */
struct ssv_dev *ssv_mac_alloc(struct device *dev);
void ssv_mac_free(struct ssv_dev *sd);
int ssv_mac_register(struct ssv_dev *sd);
void ssv_mac_unregister(struct ssv_dev *sd);

/* sdio.c */
int ssv_reg_read(struct ssv_dev *sd, u32 addr, u32 *val);
int ssv_reg_write(struct ssv_dev *sd, u32 addr, u32 val);
int ssv_reg_set_bits(struct ssv_dev *sd, u32 addr, u32 set, u32 mask);
int ssv_write_data(struct ssv_dev *sd, const u8 *buf, size_t len);
int ssv_irq_mask(struct ssv_dev *sd, u8 mask);
int ssv_irq_enable(struct ssv_dev *sd);
void ssv_irq_disable(struct ssv_dev *sd);
int ssv_load_firmware(struct ssv_dev *sd);
void ssv_set_bus_clock(struct ssv_dev *sd, u32 hz);

/* hw.c */
int ssv_hw_probe(struct ssv_dev *sd);
int ssv_hw_start(struct ssv_dev *sd);
void ssv_hw_stop(struct ssv_dev *sd);
int ssv_set_channel(struct ssv_dev *sd, int ch);
int ssv_send_cmd(struct ssv_dev *sd, u8 cmd, const void *data, size_t len);
int ssv_calibrate(struct ssv_dev *sd);
void ssv_set_bssid(struct ssv_dev *sd, const u8 *bssid);
void ssv_set_slot(struct ssv_dev *sd, bool short_slot);
int ssv_set_edca(struct ssv_dev *sd, u16 ac, bool qos,
		 const struct ieee80211_tx_queue_params *p);
/* ap.c */
void ssv_ap_init(struct ssv_dev *sd);
void ssv_ap_stop(struct ssv_dev *sd);
void ssv_ap_update_beacon(struct ssv_dev *sd);
void ssv_ap_group_queued(struct ssv_dev *sd);
bool ssv_is_ap(struct ssv_dev *sd);

void ssv_set_ap_mode(struct ssv_dev *sd, bool ap);
void ssv_beacon_enable(struct ssv_dev *sd, bool on);
void ssv_beacon_timing(struct ssv_dev *sd, u16 interval, u8 dtim_period);
int ssv_beacon_set(struct ssv_dev *sd, const u8 *buf, size_t len, u8 dtim_offset);
void ssv_beacon_release(struct ssv_dev *sd);
int ssv_wsid_add(struct ssv_dev *sd, int wsid, const u8 *addr);
void ssv_wsid_del(struct ssv_dev *sd, int wsid, const u8 *addr);
void ssv_update_ctrl_rates(struct ssv_dev *sd, u32 basic_rates);
void ssv_rf_enable(struct ssv_dev *sd, bool on);
void ssv_scan_cca(struct ssv_dev *sd, bool scanning);
void ssv_rx_ba_session(struct ssv_dev *sd, const u8 *ta, u16 tid, u16 ssn);

/* tx.c */
u32 ssv_legacy_airtime(const struct ssv_rate *r, u32 len, bool short_pre);
u32 ssv_ht_airtime(u8 mcs, u32 len, bool sgi);
int ssv_tid_to_hwq(u8 tid);
bool ssv_tx_budget(struct ssv_dev *sd, int hwq, size_t len);
int ssv_tx_write(struct ssv_dev *sd, int hwq, size_t len);
int ssv_tx_init(struct ssv_dev *sd);
void ssv_tx_deinit(struct ssv_dev *sd);
void ssv_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control,
	    struct sk_buff *skb);
void ssv_tx_flush(struct ssv_dev *sd);

/* rx.c */
void ssv_rx_irq(struct ssv_dev *sd);

/* ampdu.c */
void ssv_agg_init(struct ssv_sta *ss);
void ssv_tx_kick(struct ssv_dev *sd);
void ssv_agg_flush(struct ssv_dev *sd, struct ssv_sta *ss, u8 tid);
bool ssv_agg_tx(struct ssv_dev *sd, struct ieee80211_sta *sta, struct sk_buff *skb);
bool ssv_agg_pump(struct ssv_dev *sd, bool *blocked);
void ssv_agg_ba(struct ssv_dev *sd, struct sk_buff *skb);
void ssv_agg_no_ba(struct ssv_dev *sd, const u8 *data, size_t len);
int ssv_agg_action(struct ssv_dev *sd, struct ieee80211_vif *vif,
		   struct ieee80211_ampdu_params *params);

/* rc.c */
bool ssv_rc_agg_chain(struct ssv_dev *sd, struct ssv_sta *ss, u8 *chain);
void ssv_rc_agg_result(struct ssv_dev *sd, struct ssv_sta *ss, u8 rate,
		       int frames, int acked, int tries);
void ssv_rc_init(struct ssv_dev *sd, struct ieee80211_sta *sta);
u8 ssv_rc_get(struct ssv_dev *sd, struct ssv_sta *ss, bool *report);
void ssv_rc_report(struct ssv_dev *sd, const struct ssv_rc_report *rpt);

#endif
