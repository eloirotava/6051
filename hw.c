// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SSV6051 chip setup: e-fuse, RF/PHY tables, MAC init, calibration,
 * channel switching and per-BSS registers.
 */
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/unaligned.h>

#include "ssv6051.h"
#include "tables.h"

#define EFUSE_ID_READ_SWITCH	0xc2000128
#define EFUSE_ID_RAW_DATA	0xc200014c
#define EFUSE_READ_SWITCH	0xc200012c
#define EFUSE_RAW_DATA		0xc2000150
#define EFUSE_SECTIONS		((256 - 32) >> 5)
#define EFUSE_ITEM_MAC		3

#define OPMODE_STA		0
#define RX_11B_CCA_IN_SCAN	0x20230050

static const u32 ch_cfg_addr_p[] = { ADR_ABB_REGISTER_1, ADR_RX_ADC_REGISTER };
static const u32 ch_cfg_ch13_p[] = { 0x151559fc, 0x20d000d2 };
static u32 ch_cfg_ch1_p[ARRAY_SIZE(ch_cfg_addr_p)];

static int ssv_write_table(struct ssv_dev *sd, const struct ssv_reg *t, size_t n)
{
	size_t i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = ssv_reg_write(sd, t[i].addr, t[i].data);
		if (ret)
			return ret;
	}
	return 0;
}

static void ssv_patch_table(struct ssv_reg *t, size_t n, u32 addr, u32 data)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (t[i].addr == addr)
			t[i].data = data;
}

/*
 * The e-fuse holds a bit-packed list of items (4-bit id + payload); only
 * the MAC address is used.  The chip identity sits in its own register.
 */
static void ssv_read_efuse(struct ssv_dev *sd)
{
	u8 map[EFUSE_SECTIONS * 4];
	u32 val, pos = 0;
	int i;

	ssv_reg_write(sd, 0xc0000328, 0x11);
	ssv_reg_write(sd, EFUSE_ID_READ_SWITCH, 1);
	ssv_reg_read(sd, EFUSE_ID_RAW_DATA, &val);
	sd->chip_id = val;

	memset(map, 0, sizeof(map));
	ssv_reg_write(sd, EFUSE_READ_SWITCH, 1);
	ssv_reg_read(sd, EFUSE_RAW_DATA, &val);
	if (val) {
		for (i = 0; i < EFUSE_SECTIONS; i++) {
			ssv_reg_write(sd, EFUSE_READ_SWITCH + i * 4, 1);
			ssv_reg_read(sd, EFUSE_RAW_DATA + i * 4, &val);
			put_unaligned_le32(val, map + i * 4);
		}
	}
	ssv_reg_write(sd, 0xc0000328, 0x1800000a);

	eth_zero_addr(sd->mac);
	/* items: 4-bit id, then 8 (most) or 48 (MAC) bits of payload */
	while (map[0] && pos + 4 + 48 <= sizeof(map) * 8) {
		u8 id = (get_unaligned_le16(map + pos / 8) >> (pos % 8)) & 0xf;
		int j;

		if (id == 0 || id > 6)
			break;
		pos += 4;
		if (id == EFUSE_ITEM_MAC) {
			for (j = 0; j < ETH_ALEN; j++, pos += 8)
				sd->mac[j] = get_unaligned_le16(map + pos / 8) >> (pos % 8);
		} else {
			pos += 8;
		}
	}

	if (!is_valid_ether_addr(sd->mac)) {
		eth_random_addr(sd->mac);
		dev_warn(sd->dev, "no MAC in e-fuse, using random %pM\n", sd->mac);
	}
}

int ssv_hw_probe(struct ssv_dev *sd)
{
	size_t nrf = ARRAY_SIZE(asic_rf_setting);
	size_t nphy = ARRAY_SIZE(phy_setting);
	u32 val, gain = 0;
	size_t i;
	int ret;

	ssv_read_efuse(sd);
	dev_info(sd->dev, "chip 0x%08x, MAC %pM\n", sd->chip_id, sd->mac);

	switch (sd->chip_id) {
	case CHIP_ID_6051Q_P1:
	case CHIP_ID_6051Q_P2:
	case CHIP_ID_6051Q:
		ssv_patch_table(asic_rf_setting, nrf, 0xce010008, 0x008df61b);
		ssv_patch_table(asic_rf_setting, nrf, 0xce010014, 0x3d3e84fe);
		ssv_patch_table(asic_rf_setting, nrf, 0xce010018, 0x01457d79);
		ssv_patch_table(asic_rf_setting, nrf, 0xce01001c, 0x000103a7);
		ssv_patch_table(asic_rf_setting, nrf, 0xce010020, 0x000103a6);
		ssv_patch_table(asic_rf_setting, nrf, 0xce01002c, 0x00032ca8);
		ssv_patch_table(asic_rf_setting, nrf, 0xce010048, 0xfccccf27);
		ssv_patch_table(asic_rf_setting, nrf, 0xce010050, 0x0047c000);
		gain = 0x5b606c72;
		break;
	case CHIP_ID_6051Z:
		ssv_patch_table(asic_rf_setting, nrf, 0xce010008, 0x004d561c);
		ssv_patch_table(asic_rf_setting, nrf, 0xce010014, 0x3d9e84fe);
		ssv_patch_table(asic_rf_setting, nrf, 0xce010018, 0x00457d79);
		ssv_patch_table(asic_rf_setting, nrf, 0xce01001c, 0x000103eb);
		ssv_patch_table(asic_rf_setting, nrf, 0xce010020, 0x000103ea);
		ssv_patch_table(asic_rf_setting, nrf, 0xce01002c, 0x00062ca8);
		ssv_patch_table(asic_rf_setting, nrf, 0xce010048, 0xfccccf27);
		ssv_patch_table(asic_rf_setting, nrf, 0xce010050, 0x0047c000);
		gain = 0x60606060;
		break;
	case CHIP_ID_6051P:
		ssv_patch_table(asic_rf_setting, nrf, 0xce010008, 0x008b7c1c);
		ssv_patch_table(asic_rf_setting, nrf, 0xce010014, 0x3d7e84fe);
		ssv_patch_table(asic_rf_setting, nrf, 0xce010018, 0x01457d79);
		ssv_patch_table(asic_rf_setting, nrf, 0xce01001c, 0x000103eb);
		ssv_patch_table(asic_rf_setting, nrf, 0xce010020, 0x000103ea);
		ssv_patch_table(asic_rf_setting, nrf, 0xce01002c, 0x00032ca8);
		ssv_patch_table(asic_rf_setting, nrf, 0xce010048, 0xfccccc27);
		ssv_patch_table(asic_rf_setting, nrf, 0xce010050, 0x0047c000);
		ssv_patch_table(asic_rf_setting, nrf, 0xc0001d00, 0x5e000040);
		gain = 0x6c726c72;
		break;
	default:
		dev_warn(sd->dev, "unknown chip identity, using default RF settings\n");
	}

	if (sd->xtal == SSV_XTAL_24M) {
		ssv_patch_table(asic_rf_setting, nrf, ADR_SX_ENABLE_REGISTER, 0x0003e07c);
		ssv_patch_table(asic_rf_setting, nrf, ADR_DPLL_DIVIDER_REGISTER, 0x00406000);
		ssv_patch_table(asic_rf_setting, nrf, ADR_DPLL_FB_DIVIDER_REGISTERS_I, 0x28);
		ssv_patch_table(asic_rf_setting, nrf, ADR_DPLL_FB_DIVIDER_REGISTERS_II, 0);
	}

	for (i = 0; i < nrf; i++)
		if (asic_rf_setting[i].addr == ADR_PMU_2)
			asic_rf_setting[i].data = (asic_rf_setting[i].data & ~1) |
						  (sd->ldo ? 0 : 1);

	/* TX gain: 16 bits for 11b, 16 bits for 11g/n */
	for (i = 0; i < nphy; i++) {
		if (phy_setting[i].addr != ADR_TX_GAIN_FACTOR)
			continue;
		if (gain)
			phy_setting[i].data = gain;
		if (sd->tx_gain_b >= ARRAY_SIZE(wifi_tx_gain) ||
		    sd->tx_gain_gn >= ARRAY_SIZE(wifi_tx_gain)) {
			dev_warn(sd->dev, "TX gain level must be 0..%zu\n",
				 ARRAY_SIZE(wifi_tx_gain) - 1);
			sd->tx_gain_b = sd->tx_gain_gn = 0;
		}
		if (sd->tx_gain_b)
			phy_setting[i].data = (phy_setting[i].data & 0xffff0000) |
					      (wifi_tx_gain[sd->tx_gain_b] & 0xffff);
		if (sd->tx_gain_gn)
			phy_setting[i].data = (phy_setting[i].data & 0xffff) |
					      (wifi_tx_gain[sd->tx_gain_gn] & 0xffff0000);
	}

	ret = ssv_write_table(sd, asic_rf_setting, nrf);
	if (!ret)
		ret = ssv_reg_write(sd, ADR_PHY_EN_1, 0);
	if (ret)
		return ret;

	ret = ssv_reg_read(sd, ADR_PHY_EN_0, &val);
	if (!ret && !(val & BIT(RG_RF_BB_CLK_SEL_SFT))) {
		/* reset the baseband PLL */
		ssv_reg_read(sd, ADR_DPLL_CP_PFD_REGISTER, &val);
		val |= BIT(RG_DP_BBPLL_PD_SFT) | BIT(RG_DP_BBPLL_SDM_EDGE_SFT);
		ssv_reg_write(sd, ADR_DPLL_CP_PFD_REGISTER, val);
		val &= ~(BIT(RG_DP_BBPLL_PD_SFT) | BIT(RG_DP_BBPLL_SDM_EDGE_SFT));
		ssv_reg_write(sd, ADR_DPLL_CP_PFD_REGISTER, val);
		mdelay(10);
	}

	ret = ssv_write_table(sd, phy_setting, nphy);
	if (ret)
		return ret;
	ssv_reg_write(sd, ADR_TRX_DUMMY_REGISTER, 0xeaaaaaaa);
	ssv_reg_read(sd, ADR_TRX_DUMMY_REGISTER, &val);
	if (val != 0xeaaaaaaa) {
		dev_err(sd->dev, "register loopback failed (0x%08x)\n", val);
		return -EIO;
	}
	ssv_reg_write(sd, ADR_PAD53, 0x21);
	ssv_reg_write(sd, ADR_PAD54, 0x3000);
	ssv_reg_write(sd, ADR_PIN_SEL_0, 0x4000);
	ssv_reg_write(sd, 0xc0000304, 0x01);
	ssv_reg_write(sd, 0xc0000308, 0x01);
	ssv_reg_write(sd, ADR_CLOCK_SELECTION, 0x3);
	ssv_reg_write(sd, ADR_TRX_DUMMY_REGISTER, 0xaaaaaaaa);

	ret = ssv_set_channel(sd, 6);
	if (ret)
		return ret;
	return ssv_reg_write(sd, ADR_PHY_EN_1,
			     RG_PHYRX_MD_EN_MSK | RG_PHYTX_MD_EN_MSK |
			     RG_PHY11GN_MD_EN_MSK | RG_PHY11B_MD_EN_MSK |
			     RG_PHYRXFIFO_MD_EN_MSK | RG_PHYTXFIFO_MD_EN_MSK |
			     RG_PHY11BGN_MD_EN_MSK);
}

void ssv_rf_enable(struct ssv_dev *sd, bool on)
{
	ssv_reg_set_bits(sd, 0xce010000, (on ? 0x02 : 0x01) << 12, 0x03 << 12);
}

int ssv_set_channel(struct ssv_dev *sd, int ch)
{
	const struct ssv_chan_cal *cal;
	u32 val;
	int fails, tries, ret, i;

	if (ch < 1 || ch > 14)
		return -EINVAL;
	cal = &chan_cal[sd->xtal][ch - 1];

	if (sd->chip_id == CHIP_ID_6051P) {
		bool high = ch >= 13;

		if (high != sd->ch13_14) {
			for (i = 0; i < ARRAY_SIZE(ch_cfg_addr_p); i++)
				ssv_reg_write(sd, ch_cfg_addr_p[i], high ?
					      ch_cfg_ch13_p[i] : ch_cfg_ch1_p[i]);
			sd->ch13_14 = high;
		}
	}

	ssv_rf_enable(sd, false);
	for (fails = 0; fails < 100; fails++) {
		ret = ssv_reg_set_bits(sd, ADR_SYN_DIV_SDM_XOSC,
				       sd->xtal == SSV_XTAL_40M ? BIT(13) : 0, BIT(13));
		ret = ret ?: ssv_reg_set_bits(sd, ADR_SX_LCK_BIN_REGISTERS_I,
					      BIT(19), BIT(19));
		ret = ret ?: ssv_reg_set_bits(sd, ADR_SYN_REGISTER_1,
					      cal->rf_ctrl_f, 0x00ffffff);
		ret = ret ?: ssv_reg_set_bits(sd, ADR_SYN_REGISTER_2,
					      cal->rf_ctrl_n, 0x07ff);
		ret = ret ?: ssv_reg_set_bits(sd, ADR_SX_LCK_BIN_REGISTERS_II,
					      cal->precision, 0x1fff);
		ret = ret ?: ssv_reg_set_bits(sd, ADR_MANUAL_ENABLE_REGISTER, 0, BIT(14));
		ret = ret ?: ssv_reg_set_bits(sd, ADR_MANUAL_ENABLE_REGISTER,
					      BIT(14), BIT(14));
		if (ret)
			return ret;
		for (tries = 0; tries < 20; tries++) {
			mdelay(1);
			ret = ssv_reg_read(sd, ADR_READ_ONLY_FLAGS_1, &val);
			if (ret)
				return ret;
			if (val & BIT(1)) {
				ssv_rf_enable(sd, true);
				sd->channel = ch;
				return 0;
			}
		}
		dev_warn(sd->dev, "channel %d: synthesizer did not lock (%d)\n",
			 ch, fails + 1);
	}
	return -EIO;
}

int ssv_send_cmd(struct ssv_dev *sd, u8 cmd_id, const void *data, size_t len)
{
	size_t total = sizeof(struct ssv_host_cmd) + len;
	struct ssv_host_cmd *cmd;
	u8 *buf;
	int ret;

	buf = kzalloc(sdio_align_size(sd->func, total), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	cmd = (struct ssv_host_cmd *)buf;
	cmd->c_type = HOST_CMD;
	cmd->h_cmd = cmd_id;
	cmd->len = total;
	if (len)
		memcpy(cmd->data, data, len);
	ret = ssv_write_data(sd, buf, total);
	kfree(buf);
	return ret;
}

int ssv_calibrate(struct ssv_dev *sd)
{
	size_t nphy = sizeof(phy_setting), nrf = sizeof(asic_rf_setting);
	struct ssv_iqk_cfg cfg = {
		.cfg_xtal = sd->xtal,
		.cfg_tssi_trgt = 26,
		.cfg_tssi_div = 3,
		.cmd_sel = 0,			/* initial calibration */
		.fx_sel = 0x4 | 0x8 | 0x10 | 0x20 | 0x40 | 0x80,
		.phy_tbl_size = nphy,
		.rf_tbl_size = nrf,
	};
	size_t i;
	u8 *buf;
	int ret;
	long left;

	for (i = 0; i < ARRAY_SIZE(phy_setting); i++) {
		if (phy_setting[i].addr == ADR_TX_GAIN_FACTOR) {
			u32 g = phy_setting[i].data;

			cfg.cfg_def_tx_scale_11b = g;
			cfg.cfg_def_tx_scale_11b_p0d5 = g >> 8;
			cfg.cfg_def_tx_scale_11g = g >> 16;
			cfg.cfg_def_tx_scale_11g_p0d5 = g >> 24;
		}
	}
	if (!cfg.cfg_def_tx_scale_11b) {
		cfg.cfg_def_tx_scale_11b = 0x75;
		cfg.cfg_def_tx_scale_11b_p0d5 = 0x75;
		cfg.cfg_def_tx_scale_11g = 0x80;
		cfg.cfg_def_tx_scale_11g_p0d5 = 0x80;
	}

	buf = kmalloc(sizeof(cfg) + nphy + nrf, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	memcpy(buf, &cfg, sizeof(cfg));
	memcpy(buf + sizeof(cfg), phy_setting, nphy);
	memcpy(buf + sizeof(cfg) + nphy, asic_rf_setting, nrf);

	sd->cali_state = 0;
	ret = ssv_send_cmd(sd, SSV_CMD_INIT_CALI, buf, sizeof(cfg) + nphy + nrf);
	kfree(buf);
	if (ret)
		return ret;
	left = wait_event_timeout(sd->cali_wait, sd->cali_state != 0,
				  msecs_to_jiffies(500));
	if (!left)
		return -ETIMEDOUT;
	return sd->cali_state > 0 ? 0 : -EIO;
}

#define PBUF_NOTYPE	0
#define PBUF_TX		1	/* counted against the TX pages */

static u32 ssv_pbuf_alloc_type(struct ssv_dev *sd, u32 size, u32 type)
{
	u32 val = 0;
	int tries;

	size = round_up(size, 4);
	for (tries = 0; tries < 20; tries++) {
		ssv_reg_write(sd, ADR_WR_ALC, size | (type << 16));
		ssv_reg_read(sd, ADR_WR_ALC, &val);
		if (val)
			break;
		msleep(1);
	}
	if (!val)
		dev_err(sd->dev, "chip buffer allocation of %u bytes failed\n", size);
	return val;
}

static u32 ssv_pbuf_alloc(struct ssv_dev *sd, u32 size)
{
	return ssv_pbuf_alloc_type(sd, size, PBUF_NOTYPE);
}

static void ssv_pbuf_free(struct ssv_dev *sd, u32 addr)
{
	u32 val;
	int i;

	for (i = 0; i < 1000; i++) {
		if (ssv_reg_read(sd, ADR_MCU_STATUS, &val) || !(val & CH0_FULL_MSK))
			break;
	}
	ssv_reg_write(sd, ADR_CH0_TRIG_1, (M_ENG_TRASH_CAN << 7) | (addr >> 16));
}

static int ssv_init_mac(struct ssv_dev *sd)
{
	u32 val, tmp[8], *p;
	int i, ret;

	ssv_reg_set_bits(sd, ADR_PHY_EN_1, 0, RG_PHY_MD_EN_MSK);
	ssv_reg_write(sd, ADR_BRG_SW_RST, BIT(MAC_SW_RST_SFT));
	for (i = 0; i < 10000; i++) {
		ret = ssv_reg_read(sd, ADR_BRG_SW_RST, &val);
		if (ret)
			return ret;
		if (!val)
			break;
	}
	if (val) {
		dev_err(sd->dev, "MAC reset timed out\n");
		return -EIO;
	}

	ssv_reg_write(sd, ADR_TXQ4_MTX_Q_AIFSN, 0xffff2101);
	/* nothing of an earlier AP session may survive: DTIM hold, beacon, BSSID */
	ssv_reg_set_bits(sd, ADR_MTX_BCN_EN_MISC, 0,
			 MTX_HALT_MNG_UNTIL_DTIM_MSK | BIT(MTX_BCN_TIMER_EN_SFT));
	ssv_reg_write(sd, ADR_BSSID_0, 0);
	ssv_reg_write(sd, ADR_BSSID_1, 0);
	ssv_reg_write(sd, ADR_CONTROL, 0x12000006);
	ssv_reg_write(sd, ADR_RX_TIME_STAMP_CFG, (28 << MRX_STP_OFST_SFT) | 1);
	ssv_reg_write(sd, ADR_HCI_TX_RX_INFO_SIZE,
		      (TXPB_OFFSET << TX_PBOFFSET_SFT) |
		      (SSV_TX_DESC_LEN << TX_INFO_SIZE_SFT) |
		      (SSV_RX_DESC_LEN << RX_INFO_SIZE_SFT) |
		      (RX_PINFO_PAD << RX_LAST_PHY_SIZE_SFT));
	ssv_reg_set_bits(sd, ADR_MMU_CTRL, 0xff << MMU_SHARE_MCU_SFT,
			 0xff << MMU_SHARE_MCU_SFT);
	ssv_reg_set_bits(sd, ADR_MRX_WATCH_DOG, 0, 0xf);
	ssv_reg_read(sd, ADR_TRX_ID_THRESHOLD, &val);
	ssv_reg_write(sd, ADR_TRX_ID_THRESHOLD, (val & 0xffff0000) |
		      (HW_TX_IDS << TX_ID_THOLD_SFT) | (HW_RX_IDS << RX_ID_THOLD_SFT));
	ssv_reg_read(sd, ADR_ID_LEN_THREADSHOLD1, &val);
	ssv_reg_write(sd, ADR_ID_LEN_THREADSHOLD1, (val & 0x0f) |
		      (HW_TX_PAGES << ID_TX_LEN_THOLD_SFT) |
		      (HW_RX_PAGES << ID_RX_LEN_THOLD_SFT));
	ssv_reg_set_bits(sd, ADR_MTX_BCN_EN_MISC, BIT(MTX_TSF_TIMER_EN_SFT),
			 BIT(MTX_TSF_TIMER_EN_SFT));
	ssv_reg_write(sd, 0xcd010004, 0x1213);

	/*
	 * Chip-side buffers: the security table (unused with software
	 * crypto, but the MAC wants it) followed by the PHY info table.
	 */
	sd->key_buf[0] = ssv_pbuf_alloc(sd, sizeof(phy_info_tbl) +
					sizeof(struct ssv_hw_sec));
	for (i = 1; i < SSV_NUM_KEY_BUFS; i++)
		sd->key_buf[i] = ssv_pbuf_alloc(sd, sizeof(struct ssv_hw_sec));
	if (!sd->key_buf[0])
		return -ENOMEM;
	/* vendor quirk: keep whatever lands at 0x800e0000 allocated */
	for (i = 0; i < ARRAY_SIZE(tmp); i++)
		tmp[i] = ssv_pbuf_alloc(sd, 256);
	for (i = 0; i < ARRAY_SIZE(tmp); i++)
		if (tmp[i] && tmp[i] != 0x800e0000)
			ssv_pbuf_free(sd, tmp[i]);

	for (i = 0; i < SSV_NUM_KEY_BUFS; i++) {
		u32 x;

		for (x = 0; x < sizeof(struct ssv_hw_sec); x += 4)
			ssv_reg_write(sd, sd->key_buf[i] + x, 0);
	}
	sd->sec_buf = sd->key_buf[0];
	ssv_reg_set_bits(sd, ADR_SCRT_SET, (sd->sec_buf >> 16) << SCRT_PKT_ID_SFT,
			 ~SCRT_PKT_ID_I_MSK);

	sd->pinfo_buf = sd->sec_buf + sizeof(struct ssv_hw_sec);
	p = phy_info_tbl;
	for (i = 0; i < PHY_INFO_TBL1_SIZE; i++)
		ssv_reg_write(sd, ADR_INFO0 + i * 4, *p++);
	for (i = 0; i < PHY_INFO_TBL2_SIZE; i++)
		ssv_reg_write(sd, sd->pinfo_buf + i * 4, *p++);
	for (i = 0; i < PHY_INFO_TBL3_SIZE; i++)
		ssv_reg_write(sd, sd->pinfo_buf + (PHY_INFO_TBL2_SIZE + i) * 4, *p++);
	ssv_reg_write(sd, ADR_INFO_RATE_OFFSET, 0x00040000);
	ssv_reg_write(sd, ADR_INFO_IDX_ADDR, sd->pinfo_buf);
	ssv_reg_write(sd, ADR_INFO_LEN_ADDR, sd->pinfo_buf + PHY_INFO_TBL2_SIZE * 4);

	ssv_reg_write(sd, ADR_GLBLE_SET, (OPMODE_STA << OP_MODE_SFT) |
		      (1 << DUP_FLT_SFT) | (TX_PKT_RSVD_SETTING << TX_PKT_RSVD_SFT) |
		      (RXPB_OFFSET << PB_OFFSET_SFT));
	ssv_reg_write(sd, ADR_STA_MAC_0, get_unaligned_le32(sd->mac));
	ssv_reg_write(sd, ADR_STA_MAC_1, get_unaligned_le16(sd->mac + 4));
	ssv_set_bssid(sd, (const u8[ETH_ALEN]){ 0 });
	ssv_reg_write(sd, ADR_TX_ETHER_TYPE_0, 0);
	ssv_reg_write(sd, ADR_TX_ETHER_TYPE_1, 0);
	ssv_reg_write(sd, ADR_RX_ETHER_TYPE_0, 0);
	ssv_reg_write(sd, ADR_RX_ETHER_TYPE_1, 0);
	ssv_reg_write(sd, ADR_REASON_TRAP0, 0x7fbc7f87);
	ssv_reg_write(sd, ADR_REASON_TRAP1, 0x0000003f);
	ssv_reg_write(sd, ADR_TRAP_HW_ID, M_ENG_CPU);
	ssv_reg_write(sd, ADR_WSID0, 0);
	ssv_reg_write(sd, ADR_WSID1, 0);
	ssv_reg_write(sd, ADR_RX_FLOW_DATA, M_ENG_MACRX | (M_ENG_ENCRYPT_SEC << 4) |
		      (M_ENG_HWHCI << 8));
	ssv_reg_write(sd, ADR_RX_FLOW_MNG, M_ENG_MACRX | (M_ENG_HWHCI << 4));
	/* control frames (Block Ack) pass the MCU, which tracks aggregates */
	ssv_reg_write(sd, ADR_RX_FLOW_CTRL, M_ENG_MACRX | (M_ENG_CPU << 4) |
		      (M_ENG_HWHCI << 8));
	ssv_reg_set_bits(sd, ADR_SCRT_SET, 1 << SCRT_RPLY_IGNORE_SFT,
			 ~SCRT_RPLY_IGNORE_I_MSK);

	for (i = 0; i < DECI_TBL1_SIZE; i++)
		ssv_reg_write(sd, ADR_MRX_FLT_TB0 + i * 4, deci_tbl[i]);
	for (i = 0; i < DECI_TBL2_SIZE; i++)
		ssv_reg_write(sd, ADR_MRX_FLT_EN0 + i * 4, deci_tbl[DECI_TBL1_SIZE + i]);

	ssv_reg_set_bits(sd, ADR_GLBLE_SET, OPMODE_STA << OP_MODE_SFT, OP_MODE_MSK);
	ssv_reg_write(sd, ADR_SDIO_MASK, 0xfffe1fff);
	ssv_reg_write(sd, ADR_TX_LIMIT_INTR, 0x80000000 |
		      (TX_LOWTHRESHOLD_ID << 16) | TX_LOWTHRESHOLD_PAGE);

	ret = ssv_load_firmware(sd);
	if (ret)
		return ret;
	ssv_reg_read(sd, ADR_TX_SEG, &val);
	dev_info(sd->dev, "firmware running (version %u)\n", val);
	ssv_reg_set_bits(sd, ADR_PHY_EN_1, RG_PHY_MD_EN_MSK, RG_PHY_MD_EN_MSK);
	/* the MAC computes the FCS of each MPDU inside an aggregate */
	ssv_reg_set_bits(sd, ADR_MTX_MISC_EN, BIT(MTX_AMPDU_CRC_AUTO_SFT),
			 BIT(MTX_AMPDU_CRC_AUTO_SFT));
	return ssv_send_cmd(sd, SSV_CMD_WATCHDOG_START, NULL, 0);
}

int ssv_hw_start(struct ssv_dev *sd)
{
	int ret, i;

	ret = ssv_init_mac(sd);
	if (ret)
		return ret;

	sd->rx_ba_sta = NULL;
	/* a fresh chip has no beacon buffers */
	memset(sd->bcn_buf, 0, sizeof(sd->bcn_buf));
	memset(sd->bcn_len, 0, sizeof(sd->bcn_len));
	sd->started = true;
	ret = ssv_irq_enable(sd);
	if (ret)
		goto err;

	ret = ssv_calibrate(sd);
	if (ret) {
		dev_err(sd->dev, "calibration failed: %d\n", ret);
		goto err_irq;
	}
	ssv_reg_write(sd, ADR_PHY_EN_1, 0x217f);
	if (sd->chip_id == CHIP_ID_6051P)
		for (i = 0; i < ARRAY_SIZE(ch_cfg_addr_p); i++)
			ssv_reg_read(sd, ch_cfg_addr_p[i], &ch_cfg_ch1_p[i]);
	sd->ch13_14 = false;
	ret = ssv_set_channel(sd, sd->channel ?: 6);
	if (ret)
		goto err_irq;
	return 0;

err_irq:
	ssv_irq_disable(sd);
err:
	sd->started = false;
	return ret;
}

void ssv_hw_stop(struct ssv_dev *sd)
{
	ssv_rf_enable(sd, false);
	ssv_irq_disable(sd);
	sd->started = false;
}

void ssv_set_bssid(struct ssv_dev *sd, const u8 *bssid)
{
	ssv_reg_write(sd, ADR_BSSID_0, get_unaligned_le32(bssid));
	ssv_reg_write(sd, ADR_BSSID_1, get_unaligned_le16(bssid + 4));
}

void ssv_set_slot(struct ssv_dev *sd, bool short_slot)
{
	u32 slot = short_slot ? 9 : 20;

	ssv_reg_set_bits(sd, ADR_MTX_DUR_IFS, slot << MTX_DUR_SLOT_SFT,
			 ~MTX_DUR_SLOT_I_MSK);
	ssv_reg_set_bits(sd, ADR_MTX_DUR_SIFS_G,
			 (0xa << MTX_DUR_BURST_SIFS_G_SFT) | (slot << MTX_DUR_SLOT_G_SFT),
			 ~(MTX_DUR_BURST_SIFS_G_I_MSK & MTX_DUR_SLOT_G_I_MSK));
}

/* chip TX queue for each mac80211 AC (VO, VI, BE, BK) */
static const u8 ac_to_hwq[IEEE80211_NUM_ACS] = { 3, 2, 1, 0 };

int ssv_set_edca(struct ssv_dev *sd, u16 ac, bool qos,
		 const struct ieee80211_tx_queue_params *p)
{
	u32 cw;

	if (ac >= IEEE80211_NUM_ACS)
		return -EINVAL;
	ssv_reg_set_bits(sd, ADR_GLBLE_SET, (qos ? 1 : 0) << QOS_EN_SFT, QOS_EN_MSK);
	cw = (p->aifs - 1) & 0xf;
	cw |= (ilog2(p->cw_min + 1) & 0xf) << TXQ1_MTX_Q_ECWMIN_SFT;
	cw |= (ilog2(p->cw_max + 1) & 0xf) << TXQ1_MTX_Q_ECWMAX_SFT;
	cw |= (p->txop & 0xff) << TXQ1_MTX_Q_TXOP_LIMIT_SFT;
	return ssv_reg_write(sd, ADR_TXQ0_MTX_Q_AIFSN + 0x100 * ac_to_hwq[ac], cw);
}

static int ssv_wsid_cmd(struct ssv_dev *sd, u8 op, int wsid, const u8 *addr,
			u8 sec)
{
	struct ssv_wsid_params p = {
		.cmd = op,
		.wsid_idx = wsid,
		.hw_security = sec,
	};

	memcpy(p.target_wsid, addr, ETH_ALEN);
	return ssv_send_cmd(sd, SSV_CMD_WSID_OP, &p, sizeof(p));
}

/* Hardware station slots 0/1: the MAC ACKs and tags frames from these. */
int ssv_wsid_add(struct ssv_dev *sd, int wsid, const u8 *addr)
{
	static const u32 base[] = { ADR_WSID0, ADR_WSID1 };
	static const u32 seq[] = { ADR_WSID0_TID0_RX_SEQ, ADR_WSID1_TID0_RX_SEQ };
	static const u32 mib[] = { ADR_MTX_MIB_WSID0, ADR_MTX_MIB_WSID1 };
	int i;

	if (wsid >= SSV_NUM_HW_STA)
		return ssv_wsid_cmd(sd, SSV_WSID_OP_ADD, wsid - SSV_NUM_HW_STA, addr, 0);

	ssv_reg_write(sd, base[wsid] + 4, get_unaligned_le32(addr));
	ssv_reg_write(sd, base[wsid] + 8, get_unaligned_le16(addr + 4));
	ssv_reg_write(sd, base[wsid], 1);
	for (i = 0; i < 8; i++)
		ssv_reg_write(sd, seq[wsid] + i * 4, 0);
	ssv_reg_write(sd, mib[wsid], 0x40000000);
	ssv_wsid_cmd(sd, SSV_WSID_OP_PAIRWISE_SET_TYPE, wsid, addr, SSV_WSID_SEC_SW);
	return ssv_wsid_cmd(sd, SSV_WSID_OP_GROUP_SET_TYPE, wsid, addr, SSV_WSID_SEC_SW);
}

/*
 * Pairwise CCMP key of hardware station @wsid for receive decryption, or
 * NULL to stop.  The MAC finds the table of station N at packet buffer
 * (security buffer id + N), and inside it the N-th station entry.
 */
int ssv_set_rx_key(struct ssv_dev *sd, int wsid, const u8 *addr,
		   const struct ieee80211_key_conf *key)
{
	struct ssv_hw_sta_key k = {};
	u32 base = sd->sec_buf + (wsid << 16) +
		   offsetof(struct ssv_hw_sec, sta_key) +
		   wsid * sizeof(struct ssv_hw_sta_key);
	int i;

	if (key)
		memcpy(k.pair.key, key->key, min_t(size_t, key->keylen, sizeof(k.pair.key)));
	for (i = 0; i < sizeof(k); i += 4)
		ssv_reg_write(sd, base + i, get_unaligned_le32((u8 *)&k + i));
	ssv_reg_set_bits(sd, ADR_SCRT_SET, (key ? SSV_SEC_CCMP : SSV_SEC_NONE) << PAIR_SCRT_SFT,
			 PAIR_SCRT_MSK);
	return ssv_wsid_cmd(sd, SSV_WSID_OP_PAIRWISE_SET_TYPE, wsid, addr,
			    key ? SSV_WSID_SEC_HW : SSV_WSID_SEC_SW);
}

void ssv_wsid_del(struct ssv_dev *sd, int wsid, const u8 *addr)
{
	static const u32 base[] = { ADR_WSID0, ADR_WSID1 };

	if (wsid < SSV_NUM_HW_STA)
		ssv_reg_write(sd, base[wsid], 0);
	else
		ssv_wsid_cmd(sd, SSV_WSID_OP_DEL, wsid - SSV_NUM_HW_STA, addr, 0);
}

/* AP: operating mode, and chip queue 4 released only after DTIM beacons */
void ssv_set_ap_mode(struct ssv_dev *sd, bool ap)
{
	ssv_reg_set_bits(sd, ADR_GLBLE_SET, ap ? SSV_OPMODE_AP : SSV_OPMODE_STA,
			 OP_MODE_MSK);
	ssv_reg_set_bits(sd, ADR_MTX_BCN_EN_MISC, ap ? MTX_HALT_MNG_UNTIL_DTIM_MSK : 0,
			 MTX_HALT_MNG_UNTIL_DTIM_MSK);
}

void ssv_beacon_enable(struct ssv_dev *sd, bool on)
{
	ssv_reg_set_bits(sd, ADR_MTX_BCN_EN_MISC, on ? BIT(MTX_BCN_TIMER_EN_SFT) : 0,
			 BIT(MTX_BCN_TIMER_EN_SFT));
}

void ssv_beacon_timing(struct ssv_dev *sd, u16 interval, u8 dtim_period)
{
	ssv_reg_write(sd, ADR_MTX_BCN_PRD,
		      ((interval ?: 100) << MTX_BCN_PERIOD_SFT) |
		      ((max_t(u8, dtim_period, 1) - 1) << MTX_DTIM_NUM_SFT));
}

/*
 * The MAC sends the beacon from one of two chip buffers on its own and
 * fills in the DTIM count at @dtim_offset.  Write the new one into the
 * buffer not in use, then point the MAC at it.
 */
int ssv_beacon_set(struct ssv_dev *sd, const u8 *buf, size_t len, u8 dtim_offset)
{
	static const u32 cfg[] = { ADR_MTX_BCN_CFG0, ADR_MTX_BCN_CFG1 };
	u32 val;
	int slot, i;

	ssv_reg_write(sd, ADR_MTX_BCN_MISC, BIT(MTX_BCN_PKTID_CH_LOCK_SFT));
	ssv_reg_read(sd, ADR_MTX_BCN_MISC, &val);
	slot = ((val & MTX_BCN_CFG_VLD_MSK) >> MTX_BCN_CFG_VLD_SFT) == 1 ? 1 : 0;

	if (sd->bcn_buf[slot] && sd->bcn_len[slot] < len) {
		ssv_pbuf_free(sd, sd->bcn_buf[slot]);
		sd->bcn_buf[slot] = 0;
	}
	if (!sd->bcn_buf[slot]) {
		sd->bcn_buf[slot] = ssv_pbuf_alloc_type(sd, len, PBUF_TX);
		sd->bcn_len[slot] = len;
	}
	if (!sd->bcn_buf[slot]) {
		ssv_reg_write(sd, ADR_MTX_BCN_MISC, 0);
		return -ENOMEM;
	}
	for (i = 0; i < len; i += 4)
		ssv_reg_write(sd, sd->bcn_buf[slot] + i, get_unaligned_le32(buf + i));
	ssv_reg_write(sd, cfg[slot], ((sd->bcn_buf[slot] & 0x0fff0000) >> 16) |
		      (dtim_offset << MTX_DTIM_OFST0_SFT));
	ssv_reg_write(sd, ADR_MTX_BCN_MISC, 0);
	return 0;
}

void ssv_beacon_release(struct ssv_dev *sd)
{
	u32 val;
	int i;

	for (i = 0; i < 10; i++) {
		ssv_beacon_enable(sd, false);
		if (ssv_reg_read(sd, ADR_MTX_BCN_MISC, &val) ||
		    !(val & MTX_AUTO_BCN_ONGOING_MSK))
			break;
		msleep(1);
	}
	for (i = 0; i < ARRAY_SIZE(sd->bcn_buf); i++) {
		if (sd->bcn_buf[i])
			ssv_pbuf_free(sd, sd->bcn_buf[i]);
		sd->bcn_buf[i] = 0;
		sd->bcn_len[i] = 0;
	}
}

/* ACK/CTS rate for the CCK rates follows the BSS basic rate set. */
void ssv_update_ctrl_rates(struct ssv_dev *sd, u32 basic_rates)
{
	int i, ctrl = 0;

	for (i = 0; i < 4; i++) {
		if (basic_rates & BIT(i))
			ctrl = i;
		ssv_reg_set_bits(sd, sd->pinfo_buf + i * 4, ctrl << 4, 0x3f0);
		if (i)
			ssv_reg_set_bits(sd, sd->pinfo_buf + (i + 3) * 4, ctrl << 4, 0x3f0);
	}
}

void ssv_scan_cca(struct ssv_dev *sd, bool scanning)
{
	if (scanning) {
		ssv_reg_read(sd, ADR_RX_11B_CCA_CONTROL, &sd->cca_control);
		ssv_reg_read(sd, ADR_RX_11B_CCA_1, &sd->cca_1);
		ssv_reg_write(sd, ADR_RX_11B_CCA_CONTROL, 0);
		ssv_reg_write(sd, ADR_RX_11B_CCA_1, RX_11B_CCA_IN_SCAN);
	} else {
		ssv_reg_write(sd, ADR_RX_11B_CCA_CONTROL, sd->cca_control);
		ssv_reg_write(sd, ADR_RX_11B_CCA_1, sd->cca_1);
	}
}

/* The MAC answers aggregates with Block Ack for one (TA, TID) at a time. */
void ssv_rx_ba_session(struct ssv_dev *sd, const u8 *ta, u16 tid, u16 ssn)
{
	if (!ta) {
		ssv_reg_write(sd, ADR_BA_CTRL, 0);
		return;
	}
	ssv_reg_write(sd, ADR_BA_TA_0, get_unaligned_le32(ta));
	ssv_reg_write(sd, ADR_BA_TA_1, get_unaligned_le16(ta + 4));
	ssv_reg_write(sd, ADR_BA_TID, tid);
	ssv_reg_write(sd, ADR_BA_ST_SEQ, ssn);
	ssv_reg_write(sd, ADR_BA_SB0, 0);
	ssv_reg_write(sd, ADR_BA_SB1, 0);
	ssv_reg_write(sd, ADR_BA_CTRL, 0xb);
}
