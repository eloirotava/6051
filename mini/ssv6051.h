/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Minimal mac80211 driver for the South Silicon Valley SSV6051 SDIO
 * 802.11b/g/n chip (station mode only, software crypto).
 *
 * Hardware interface derived from the iComm vendor driver:
 * Copyright (c) 2015 South Silicon Valley Microelectronics Inc.
 * Copyright (c) 2015 iComm Corporation
 */
#ifndef SSV6051_H
#define SSV6051_H

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/skbuff.h>
#include <linux/wait.h>
#include <linux/mmc/sdio_func.h>
#include <net/mac80211.h>

#include "reg.h"
#include "aux.h"

#define SSV_FIRMWARE		"ssv6051-sw.bin"

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
#define SDIO_CLOCK_FW		25000000
#define SSV_MAX_FRAME		4096

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

#define SSV_NUM_HW_STA		2
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
	SSV_EVT_RC_MPDU_REPORT = 2,
	SSV_EVT_RC_AMPDU_REPORT = 3,
	SSV_EVT_TXLOOPBK_RESULT = 10,
};

#define SSV_TXREPORT_RC		2	/* tx_desc.RSVD_0 value asking for an RC report */

enum ssv_wsid_op {
	SSV_WSID_OP_PAIRWISE_SET_TYPE = 5,
	SSV_WSID_OP_GROUP_SET_TYPE = 6,
};
#define SSV_WSID_SEC_SW		0

/*
 * Wire formats.  All little-endian, bitfields allocated LSB first; GCC
 * lays these out identically on arm and arm64.
 */
struct ssv_rc_retry {
	u32 count:4;
	u32 drate:6;
	u32 crate:6;
	u32 rts_cts_nav:16;
	u32 frame_consume_time:10;
	u32 dl_length:12;
	u32 rsvd:10;
} __packed;

#define SSV_TX_MAX_RATES	3

struct ssv_tx_desc {
	u32 len:16;
	u32 c_type:3;
	u32 f80211:1;
	u32 qos:1;
	u32 ht:1;
	u32 use_4addr:1;
	u32 RSVD_0:3;
	u32 bc_que:1;
	u32 security:1;
	u32 more_data:1;
	u32 stype_b5b4:2;
	u32 extra_info:1;
	u32 fCmd;
	u32 hdr_offset:8;
	u32 frag:1;
	u32 unicast:1;
	u32 hdr_len:6;
	u32 tx_report:1;
	u32 tx_burst:1;
	u32 ack_policy:2;
	u32 aggregation:1;
	u32 RSVD_1:3;
	u32 do_rts_cts:2;
	u32 reason:6;
	u32 payload_offset:8;
	u32 RSVD_4:7;
	u32 RSVD_2:1;
	u32 fCmdIdx:3;
	u32 wsid:4;
	u32 txq_idx:3;
	u32 TxF_ID:6;
	u32 rts_cts_nav:16;
	u32 frame_consume_time:10;
	u32 crate_idx:6;
	u32 drate_idx:6;
	u32 dl_length:12;
	u32 RSVD_3:14;
	u32 RESERVED[8];
	struct ssv_rc_retry rc_params[SSV_TX_MAX_RATES];
};

struct ssv_rx_desc {
	u32 len:16;
	u32 c_type:3;
	u32 f80211:1;
	u32 qos:1;
	u32 ht:1;
	u32 use_4addr:1;
	u32 l3cs_err:1;
	u32 l4cs_err:1;
	u32 align2:1;
	u32 RSVD_0:2;
	u32 psm:1;
	u32 stype_b5b4:2;
	u32 extra_info:1;
	u32 edca0_used:4;
	u32 edca1_used:5;
	u32 edca2_used:5;
	u32 edca3_used:5;
	u32 mng_used:4;
	u32 tx_page_used:9;
	u32 hdr_offset:8;
	u32 frag:1;
	u32 unicast:1;
	u32 hdr_len:6;
	u32 RxResult:8;
	u32 wildcard_bssid:1;
	u32 RSVD_1:1;
	u32 reason:6;
	u32 payload_offset:8;
	u32 tx_id_used:8;
	u32 fCmdIdx:3;
	u32 wsid:4;
	u32 RSVD_3:3;
	u32 rate_idx:6;
};

struct ssv_rxphy_info {
	u32 len:16;
	u32 rsvd0:16;
	u32 mode:3;
	u32 ch_bw:3;
	u32 preamble:1;
	u32 ht_short_gi:1;
	u32 rate:7;
	u32 rsvd1:1;
	u32 smoothing:1;
	u32 no_sounding:1;
	u32 aggregate:1;
	u32 stbc:2;
	u32 fec:1;
	u32 n_ess:2;
	u32 rsvd2:8;
	u32 l_length:12;
	u32 l_rate:3;
	u32 rsvd3:17;
	u32 rsvd4;
	u32 rpci:8;
	u32 snr:8;
	u32 service:16;
};

/* Trailing PHY info appended to CCK frames */
struct ssv_rxphy_pad {
	u32 rpci:8;
	u32 snr:8;
	u32 rsvd:16;
};

#define SSV_TX_DESC_LEN		sizeof(struct ssv_tx_desc)
#define SSV_RX_DESC_LEN		(sizeof(struct ssv_rx_desc) + sizeof(struct ssv_rxphy_info))

struct ssv_host_cmd {
	u32 len:16;
	u32 c_type:3;
	u32 rsvd:5;
	u32 h_cmd:8;
	u32 seq_no;
	u8 data[];
};

struct ssv_host_event {
	u32 len:16;
	u32 c_type:3;
	u32 rsvd:5;
	u32 h_event:8;
	u32 seq_no;
	u8 data[];
};

struct ssv_tx_rate_rpt {
	s8 data_rate;
	u8 count;
} __packed;

struct ssv_rc_report {
	u8 wsid;
	struct ssv_tx_rate_rpt rates[SSV_TX_MAX_RATES];
	u16 ampdu_len;
	u16 ampdu_ack_len;
	int ack_signal;
} __packed;

struct ssv_wsid_params {
	u8 cmd;
	u8 wsid_idx;
	u8 target_wsid[6];
	u8 hw_security;
};

struct ssv_iqk_cfg {
	u32 cfg_xtal:8;
	u32 cfg_pa:8;
	u32 cfg_pabias_ctrl:8;
	u32 cfg_pacascode_ctrl:8;
	u32 cfg_tssi_trgt:8;
	u32 cfg_tssi_div:8;
	u32 cfg_def_tx_scale_11b:8;
	u32 cfg_def_tx_scale_11b_p0d5:8;
	u32 cfg_def_tx_scale_11g:8;
	u32 cfg_def_tx_scale_11g_p0d5:8;
	u32 cmd_sel;
	u32 fx_sel;
	u32 phy_tbl_size;
	u32 rf_tbl_size;
};

/* Chip packet-buffer security table (content unused with software crypto) */
struct ssv_hw_key {
	u8 key[32];
	u32 tx_pn_l;
	u32 tx_pn_h;
	u32 rx_pn_l;
	u32 rx_pn_h;
} __packed;

struct ssv_hw_sta_key {
	u8 pair_key_idx:4;
	u8 group_key_idx:4;
	u8 valid;
	u8 reserve[2];
	struct ssv_hw_key pair;
} __packed;

struct ssv_hw_sec {
	struct ssv_hw_key group_key[3];
	struct ssv_hw_sta_key sta_key[8];
} __packed;

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
	u32 prob[SSV_RC_MAX];	/* EWMA success probability, 0..1024 */
	u16 att[SSV_RC_MAX];
	u16 ok[SSV_RC_MAX];
	u32 frames;
	unsigned long last_update;
};

struct ssv_sta {
	int wsid;
	struct ssv_rc rc;
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
	u32 sdio_clock;
	u32 chip_id;
	u8 mac[ETH_ALEN];

	/* SDIO */
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

	/* calibration handshake */
	wait_queue_head_t cali_wait;
	int cali_state;

	/* association */
	struct mutex mutex;
	spinlock_t sta_lock;
	struct ieee80211_vif *vif;
	struct ieee80211_sta __rcu *sta[SSV_NUM_HW_STA];
	bool short_preamble;
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
int ssv_wsid_add(struct ssv_dev *sd, int wsid, const u8 *addr);
void ssv_wsid_del(struct ssv_dev *sd, int wsid);
void ssv_update_ctrl_rates(struct ssv_dev *sd, u32 basic_rates);
void ssv_rf_enable(struct ssv_dev *sd, bool on);
void ssv_scan_cca(struct ssv_dev *sd, bool scanning);

/* tx.c */
int ssv_tx_init(struct ssv_dev *sd);
void ssv_tx_deinit(struct ssv_dev *sd);
void ssv_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control,
	    struct sk_buff *skb);
void ssv_tx_flush(struct ssv_dev *sd);

/* rx.c */
void ssv_rx_irq(struct ssv_dev *sd);

/* rc.c */
void ssv_rc_init(struct ssv_dev *sd, struct ieee80211_sta *sta);
u8 ssv_rc_get(struct ssv_dev *sd, struct ssv_sta *ss, bool *report);
void ssv_rc_report(struct ssv_dev *sd, const struct ssv_rc_report *rpt);

#endif
