/*
 * Copyright (c) 2015 South Silicon Valley Microelectronics Inc.
 * Copyright (c) 2015 iComm Corporation
 *
 * This program is free software: you can redistribute it and/or modify 
 * it under the terms of the GNU General Public License as published by 
 * the Free Software Foundation, either version 3 of the License, or 
 * (at your option) any later version.
 * This program is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  
 * See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License 
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* Only the bit fields the driver uses (trimmed from the vendor dump). */
#define MAC_SW_RST_SFT 1
#define TX_PBOFFSET_SFT 0
#define TX_INFO_SIZE_SFT 8
#define RX_INFO_SIZE_SFT 16
#define RX_LAST_PHY_SIZE_SFT 24
#define MRX_STP_OFST_SFT 8
#define MTX_AMPDU_CRC_AUTO_SFT 5
#define MTX_BCN_TIMER_EN_I_MSK 0xfffffffe
#define MTX_BCN_TIMER_EN_SFT 0
#define MTX_TSF_TIMER_EN_SFT 5
#define MTX_HALT_MNG_UNTIL_DTIM_MSK 0x00000040
#define MTX_HALT_MNG_UNTIL_DTIM_SFT 6
#define MTX_BCN_PKTID_CH_LOCK_SFT 0
#define MTX_BCN_CFG_VLD_MSK 0x00000006
#define MTX_BCN_CFG_VLD_SFT 1
#define MTX_AUTO_BCN_ONGOING_MSK 0x00000008
#define MTX_AUTO_BCN_ONGOING_SFT 3
#define MTX_BCN_PERIOD_SFT 0
#define MTX_DTIM_NUM_SFT 24
#define MTX_DTIM_OFST0_SFT 16
#define MTX_DUR_SLOT_I_MSK 0xffc0ffff
#define MTX_DUR_SLOT_SFT 16
#define MTX_DUR_BURST_SIFS_G_I_MSK 0xffff00ff
#define MTX_DUR_BURST_SIFS_G_SFT 8
#define MTX_DUR_SLOT_G_I_MSK 0xffc0ffff
#define MTX_DUR_SLOT_G_SFT 16
#define TXQ1_MTX_Q_ECWMIN_SFT 8
#define TXQ1_MTX_Q_ECWMAX_SFT 12
#define TXQ1_MTX_Q_TXOP_LIMIT_SFT 16
#define OP_MODE_MSK 0x00000003
#define OP_MODE_SFT 0
#define QOS_EN_MSK 0x00000010
#define QOS_EN_SFT 4
#define PB_OFFSET_SFT 8
#define SNIFFER_MODE_SFT 16
#define DUP_FLT_SFT 17
#define TX_PKT_RSVD_SFT 18
#define PAIR_SCRT_MSK 0x00000007
#define PAIR_SCRT_I_MSK 0xfffffff8
#define PAIR_SCRT_SFT 0
#define GRP_SCRT_I_MSK 0xffffffc7
#define GRP_SCRT_SFT 3
#define SCRT_PKT_ID_I_MSK 0xffffe03f
#define SCRT_PKT_ID_SFT 6
#define SCRT_RPLY_IGNORE_I_MSK 0xfffeffff
#define SCRT_RPLY_IGNORE_SFT 16
#define CH0_FULL_MSK 0x00000001
#define TX_ID_THOLD_SFT 0
#define RX_ID_THOLD_SFT 8
#define ID_TX_LEN_THOLD_SFT 4
#define ID_RX_LEN_THOLD_SFT 13
#define RG_RF_BB_CLK_SEL_SFT 31
#define RG_PHY_MD_EN_MSK 0x00000001
#define RG_PHY_MD_EN_SFT 0
#define RG_PHYRX_MD_EN_MSK 0x00000002
#define RG_PHYTX_MD_EN_MSK 0x00000004
#define RG_PHY11GN_MD_EN_MSK 0x00000008
#define RG_PHY11B_MD_EN_MSK 0x00000010
#define RG_PHYRXFIFO_MD_EN_MSK 0x00000020
#define RG_PHYTXFIFO_MD_EN_MSK 0x00000040
#define RG_PHY11BGN_MD_EN_MSK 0x00000100
#define RG_TX_GAIN_OFFSET_I_MSK 0xf87fffff
#define RG_TX_GAIN_OFFSET_SFT 23
#define RG_SARADC_THERMAL_MSK 0x04000000
#define RG_SARADC_THERMAL_SFT 26
#define RG_EN_SARADC_MSK 0x40000000
#define RG_EN_SARADC_SFT 30
#define RG_XOSC_CBANK_XO_I_MSK 0xfff87fff
#define RG_XOSC_CBANK_XO_SFT 15
#define RG_DP_BBPLL_PD_SFT 9
#define RG_DP_BBPLL_SDM_EDGE_SFT 31
#define RG_SARADC_BIT_MSK 0x003f0000
#define RG_SARADC_BIT_SFT 16
#define SAR_ADC_FSM_RDY_MSK 0x00400000
#define SAR_ADC_FSM_RDY_SFT 22
#define MMU_SHARE_MCU_SFT 16
