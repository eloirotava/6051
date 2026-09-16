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

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <ssv6200.h>
#include "hctrl.h"

static struct ssv6xxx_hci_ctrl *ctrl_hci = NULL;

struct sk_buff *ssv_skb_alloc(s32 len)
{
	struct sk_buff *skb;
	skb = __dev_alloc_skb(len + SSV6200_ALLOC_RSVD, GFP_KERNEL);
	if (skb != NULL) {
		skb_reserve(skb, SSV_SKB_info_size);
	}
	return skb;
}

void ssv_skb_free(struct sk_buff *skb)
{
	dev_kfree_skb_any(skb);
}

static int ssv6xxx_hci_irq_enable(void)
{
	HCI_IRQ_SET_MASK(ctrl_hci, ~(ctrl_hci->int_mask));
	HCI_IRQ_ENABLE(ctrl_hci);
	return 0;
}

static int ssv6xxx_hci_irq_disable(void)
{
	HCI_IRQ_SET_MASK(ctrl_hci, 0xffffffff);
	HCI_IRQ_DISABLE(ctrl_hci);
	return 0;
}

static void ssv6xxx_hci_irq_register(u32 irq_mask)
{
	unsigned long flags;
	u32 regval;
	mutex_lock(&ctrl_hci->hci_mutex);
	spin_lock_irqsave(&ctrl_hci->int_lock, flags);
	ctrl_hci->int_mask |= irq_mask;
	regval = ~ctrl_hci->int_mask;
	spin_unlock_irqrestore(&ctrl_hci->int_lock, flags);
	smp_mb();
	HCI_IRQ_SET_MASK(ctrl_hci, regval);
	mutex_unlock(&ctrl_hci->hci_mutex);
}

static inline u32 ssv6xxx_hci_get_int_bitno(int txqid)
{
	if (txqid == SSV_HW_TXQ_NUM - 1)
		return 1;
	else
		return txqid + 3;
}

static int ssv6xxx_hci_start(void)
{
	ssv6xxx_hci_irq_enable();
	ctrl_hci->hci_start = true;
	HCI_IRQ_TRIGGER(ctrl_hci);
	return 0;
}

static int ssv6xxx_hci_stop(void)
{
	ssv6xxx_hci_irq_disable();
	ctrl_hci->hci_start = false;
	return 0;
}

static int ssv6xxx_hci_read_word(u32 addr, u32 * regval)
{
	int ret = HCI_REG_READ(ctrl_hci, addr, regval);
	return ret;
}

static int ssv6xxx_hci_write_word(u32 addr, u32 regval)
{
	return HCI_REG_WRITE(ctrl_hci, addr, regval);
}

static int ssv6xxx_hci_load_fw(u8 * firmware_name, u8 openfile)
{
	return HCI_LOAD_FW(ctrl_hci, firmware_name, openfile);
}

static int ssv6xxx_hci_write_sram(u32 addr, u8 * data, u32 size)
{
	return HCI_SRAM_WRITE(ctrl_hci, addr, data, size);
}

static int ssv6xxx_hci_pmu_wakeup(void)
{
	HCI_PMU_WAKEUP(ctrl_hci);
	return 0;
}

static int ssv6xxx_hci_interface_reset(void)
{
	HCI_IFC_RESET(ctrl_hci);
	return 0;
}

static int ssv6xxx_hci_send_cmd(struct sk_buff *skb)
{
	int ret;
	ret = IF_SEND(ctrl_hci, (void *)skb->data, skb->len, 0);

	if (ret < 0)
		pr_warn("ssv6xxx_hci_send_cmd failed, ret=%d\n", ret);

	return ret;
}

static int ssv6xxx_hci_enqueue(struct sk_buff *skb, int txqid, u32 tx_flags)
{
	struct ssv_hw_txq *hw_txq;
	unsigned long flags;
	u32 status;
	int qlen = 0;
	BUG_ON(txqid >= SSV_HW_TXQ_NUM || txqid < 0);
	if (txqid >= SSV_HW_TXQ_NUM || txqid < 0)
		return -1;
	hw_txq = &ctrl_hci->hw_txq[txqid];
	hw_txq->tx_flags = tx_flags;
	if (tx_flags & HCI_FLAGS_ENQUEUE_HEAD)
		skb_queue_head(&hw_txq->qhead, skb);
	else
		skb_queue_tail(&hw_txq->qhead, skb);
	qlen = (int)skb_queue_len(&hw_txq->qhead);
	if (!(tx_flags & HCI_FLAGS_NO_FLOWCTRL)) {
		if (skb_queue_len(&hw_txq->qhead) >= hw_txq->max_qsize) {
			ctrl_hci->shi->hci_tx_flow_ctrl_cb(ctrl_hci->
							   shi->tx_fctrl_cb_args,
							   hw_txq->txq_no, true,
							   2000);
		}
	}

	mutex_lock(&ctrl_hci->hci_mutex);
	spin_lock_irqsave(&ctrl_hci->int_lock, flags);
	status = ctrl_hci->int_mask;

	if ((ctrl_hci->int_mask & SSV6XXX_INT_RESOURCE_LOW) == 0) {
		if (ctrl_hci->shi->if_ops->trigger_tx_rx == NULL) {
			u32 regval;
			ctrl_hci->int_mask |= SSV6XXX_INT_RESOURCE_LOW;
			regval = ~ctrl_hci->int_mask;
			spin_unlock_irqrestore(&ctrl_hci->int_lock, flags);
			HCI_IRQ_SET_MASK(ctrl_hci, regval);
			mutex_unlock(&ctrl_hci->hci_mutex);
		} else {
			ctrl_hci->int_status |= SSV6XXX_INT_RESOURCE_LOW;
			smp_mb();
			spin_unlock_irqrestore(&ctrl_hci->int_lock, flags);
			mutex_unlock(&ctrl_hci->hci_mutex);
			ctrl_hci->shi->if_ops->trigger_tx_rx(ctrl_hci->
							     shi->dev);
		}
	} else {
		spin_unlock_irqrestore(&ctrl_hci->int_lock, flags);
		mutex_unlock(&ctrl_hci->hci_mutex);
	}

	return qlen;
}

static bool ssv6xxx_hci_is_txq_empty(int txqid)
{
	struct ssv_hw_txq *hw_txq;
	BUG_ON(txqid >= SSV_HW_TXQ_NUM);
	if (txqid >= SSV_HW_TXQ_NUM)
		return false;
	hw_txq = &ctrl_hci->hw_txq[txqid];
	if (skb_queue_len(&hw_txq->qhead) <= 0)
		return true;
	return false;
}

static int ssv6xxx_hci_txq_flush(u32 txq_mask)
{
	struct ssv_hw_txq *hw_txq;
	struct sk_buff *skb = NULL;
	int txqid;
	for (txqid = 0; txqid < SSV_HW_TXQ_NUM; txqid++) {
		if ((txq_mask & (1 << txqid)) != 0)
			continue;
		hw_txq = &ctrl_hci->hw_txq[txqid];
		while ((skb = skb_dequeue(&hw_txq->qhead))) {
			ctrl_hci->shi->hci_tx_buf_free_cb(skb,
							  ctrl_hci->
							  shi->tx_buf_free_args);
		}
	}
	return 0;
}

static int ssv6xxx_hci_txq_flush_by_sta(int aid)
{
	return 0;
}

static int ssv6xxx_hci_txq_pause(u32 txq_mask)
{
	struct ssv_hw_txq *hw_txq;
	int txqid;
	mutex_lock(&ctrl_hci->txq_mask_lock);
	ctrl_hci->txq_mask |= (txq_mask & 0x1F);
	for (txqid = 0; txqid < SSV_HW_TXQ_NUM; txqid++) {
		if ((ctrl_hci->txq_mask & (1 << txqid)) == 0)
			continue;
		hw_txq = &ctrl_hci->hw_txq[txqid];
		hw_txq->paused = true;
	}
	HCI_REG_SET_BITS(ctrl_hci, ADR_MTX_MISC_EN,
			 (ctrl_hci->txq_mask << 16), (0x1F << 16));
	mutex_unlock(&ctrl_hci->txq_mask_lock);
	return 0;
}

static int ssv6xxx_hci_txq_resume(u32 txq_mask)
{
	struct ssv_hw_txq *hw_txq;
	int txqid;
	mutex_lock(&ctrl_hci->txq_mask_lock);
	ctrl_hci->txq_mask &= ~(txq_mask & 0x1F);
	for (txqid = 0; txqid < SSV_HW_TXQ_NUM; txqid++) {
		if ((ctrl_hci->txq_mask & (1 << txqid)) != 0)
			continue;
		hw_txq = &ctrl_hci->hw_txq[txqid];
		hw_txq->paused = false;
	}
	HCI_REG_SET_BITS(ctrl_hci, ADR_MTX_MISC_EN,
			 (ctrl_hci->txq_mask << 16), (0x1F << 16));
	mutex_unlock(&ctrl_hci->txq_mask_lock);
	return 0;
}

static int ssv6xxx_hci_xmit(struct ssv_hw_txq *hw_txq, int max_count,
			    struct ssv6xxx_hw_resource *phw_resource)
{
	struct sk_buff_head tx_cb_list;
	struct sk_buff *skb = NULL;
	int tx_count, ret, page_count;
	struct ssv6200_tx_desc *tx_desc = NULL;
	ctrl_hci->xmit_running = 1;
	skb_queue_head_init(&tx_cb_list);
	for (tx_count = 0; tx_count < max_count; tx_count++) {
		if (ctrl_hci->hci_start == false) {
			pr_debug("ssv6xxx_hci_xmit - hci_start = false\n");
			goto xmit_out;
		}
		skb = skb_dequeue(&hw_txq->qhead);
		if (!skb) {
			pr_debug("ssv6xxx_hci_xmit - queue empty\n");
			goto xmit_out;
		}
		page_count = (skb->len + SSV6200_ALLOC_RSVD);
		if (page_count & HW_MMU_PAGE_MASK)
			page_count = (page_count >> HW_MMU_PAGE_SHIFT) + 1;
		else
			page_count = page_count >> HW_MMU_PAGE_SHIFT;
		if (page_count > (SSV6200_PAGE_TX_THRESHOLD / 2))
			pr_err("Asking page %d(%d) exceeds resource limit %d.\n",
			       page_count, skb->len,
			       (SSV6200_PAGE_TX_THRESHOLD / 2));
		if ((phw_resource->free_tx_page < page_count)
		    || (phw_resource->free_tx_id <= 0)
		    || (phw_resource->max_tx_frame[hw_txq->txq_no] <= 0)) {
			skb_queue_head(&hw_txq->qhead, skb);
			break;
		}
		phw_resource->free_tx_page -= page_count;
		phw_resource->free_tx_id--;
		phw_resource->max_tx_frame[hw_txq->txq_no]--;
		tx_desc = (struct ssv6200_tx_desc *)skb->data;

		if (ctrl_hci->shi->hci_skb_update_cb != NULL
		    && tx_desc->reason != ID_TRAP_SW_TXTPUT) {
			ctrl_hci->shi->hci_skb_update_cb(skb,
							 ctrl_hci->
							 shi->skb_update_args);
		}

		ret =
		    IF_SEND(ctrl_hci, (void *)skb->data, skb->len,
			    hw_txq->txq_no);
		if (ret < 0) {
			pr_err("ssv6xxx_hci_xmit failure\n");
			skb_queue_head(&hw_txq->qhead, skb);
			break;
		}
		if (tx_desc->reason != ID_TRAP_SW_TXTPUT)
			skb_queue_tail(&tx_cb_list, skb);
		else
			ssv_skb_free(skb);
		hw_txq->tx_pkt++;

		if (!(hw_txq->tx_flags & HCI_FLAGS_NO_FLOWCTRL)) {
			if (skb_queue_len(&hw_txq->qhead) < hw_txq->resum_thres) {
				ctrl_hci->shi->
				    hci_tx_flow_ctrl_cb
				    (ctrl_hci->shi->tx_fctrl_cb_args,
				     hw_txq->txq_no, false, 2000);
			}
		}
	}
 xmit_out:
	if (ctrl_hci->shi->hci_tx_cb && tx_desc
	    && tx_desc->reason != ID_TRAP_SW_TXTPUT) {
		ctrl_hci->shi->hci_tx_cb(&tx_cb_list,
					 ctrl_hci->shi->tx_cb_args);
	}
	ctrl_hci->xmit_running = 0;
	return tx_count;
}

static int ssv6xxx_hci_tx_handler(void *dev, int max_count)
{
	struct ssv6xxx_hci_txq_info txq_info;
	struct ssv6xxx_hci_txq_info2 txq_info2;
	struct ssv6xxx_hw_resource hw_resource;
	struct ssv_hw_txq *hw_txq = dev;
	int ret, tx_count = 0;
	max_count = skb_queue_len(&hw_txq->qhead);
	if (max_count == 0)
		return 0;
	if (hw_txq->txq_no == 4) {
		ret =
		    HCI_REG_READ(ctrl_hci, ADR_TX_ID_ALL_INFO2,
				 (u32 *) & txq_info2);
		if (ret < 0) {
			ctrl_hci->read_rs1_info_fail++;
			return 0;
		}
		//BUG_ON(SSV6200_PAGE_TX_THRESHOLD < txq_info2.tx_use_page);
		//BUG_ON(SSV6200_ID_TX_THRESHOLD < txq_info2.tx_use_id);
		if (SSV6200_PAGE_TX_THRESHOLD < txq_info2.tx_use_page)
			return 0;
		if (SSV6200_ID_TX_THRESHOLD < txq_info2.tx_use_id)
			return 0;
		hw_resource.free_tx_page =
		    SSV6200_PAGE_TX_THRESHOLD - txq_info2.tx_use_page;
		hw_resource.free_tx_id =
		    SSV6200_ID_TX_THRESHOLD - txq_info2.tx_use_id;
		hw_resource.max_tx_frame[4] =
		    SSV6200_ID_MANAGER_QUEUE - txq_info2.txq4_size;
	} else {
		ret =
		    HCI_REG_READ(ctrl_hci, ADR_TX_ID_ALL_INFO,
				 (u32 *) & txq_info);
		if (ret < 0) {
			ctrl_hci->read_rs0_info_fail++;
			return 0;
		}
		//BUG_ON(SSV6200_PAGE_TX_THRESHOLD < txq_info.tx_use_page);
		//BUG_ON(SSV6200_ID_TX_THRESHOLD < txq_info.tx_use_id);
		if (SSV6200_PAGE_TX_THRESHOLD < txq_info.tx_use_page)
			return 0;
		if (SSV6200_ID_TX_THRESHOLD < txq_info.tx_use_id)
			return 0;
		hw_resource.free_tx_page =
		    SSV6200_PAGE_TX_THRESHOLD - txq_info.tx_use_page;
		hw_resource.free_tx_id =
		    SSV6200_ID_TX_THRESHOLD - txq_info.tx_use_id;
		hw_resource.max_tx_frame[0] =
		    SSV6200_ID_AC_BK_OUT_QUEUE - txq_info.txq0_size;
		hw_resource.max_tx_frame[1] =
		    SSV6200_ID_AC_BE_OUT_QUEUE - txq_info.txq1_size;
		hw_resource.max_tx_frame[2] =
		    SSV6200_ID_AC_VI_OUT_QUEUE - txq_info.txq2_size;
		hw_resource.max_tx_frame[3] =
		    SSV6200_ID_AC_VO_OUT_QUEUE - txq_info.txq3_size;
		/* Values come from a register read over SDIO; a glitch must
		 * not take the whole kernel down.  Retry on the next IRQ. */
		if (hw_resource.max_tx_frame[3] < 0 ||
		    hw_resource.max_tx_frame[2] < 0 ||
		    hw_resource.max_tx_frame[1] < 0 ||
		    hw_resource.max_tx_frame[0] < 0) {
			ctrl_hci->read_rs0_info_fail++;
			return 0;
		}
	}
	{
		tx_count = ssv6xxx_hci_xmit(hw_txq, max_count, &hw_resource);
	}
	if ((ctrl_hci->shi->hci_tx_q_empty_cb != NULL)
	    && (skb_queue_len(&hw_txq->qhead) == 0)) {
		ctrl_hci->shi->hci_tx_q_empty_cb(hw_txq->txq_no,
						 ctrl_hci->
						 shi->tx_q_empty_args);
	}
	return tx_count;
}

void ssv6xxx_hci_tx_work(struct work_struct *work)
{
	ssv6xxx_hci_irq_register(SSV6XXX_INT_RESOURCE_LOW);
}

static int _do_rx(struct ssv6xxx_hci_ctrl *hctl, u32 isr_status)
{
	struct sk_buff_head rx_list;
	struct sk_buff *rx_mpdu;
	int rx_cnt, ret = 0;
	size_t dlen;
	u32 status = isr_status;
	skb_queue_head_init(&rx_list);
	hctl->rx_last_status_valid = false;
	for (rx_cnt = 0; (status & SSV6XXX_INT_RX) && (rx_cnt < 32); rx_cnt++) {
		/* in: room in rx_buf, out: frame length */
		dlen = MAX_FRAME_SIZE;
		ret = IF_RECV(hctl, hctl->rx_buf->data, &dlen);
		if (ret < 0 || dlen <= 0) {
			pr_warn_ratelimited("%s(): IF_RECV() returns %d (dlen=%d)\n",
			       __FUNCTION__, ret, (int)dlen);
			/* Never hand a failed/short read to the stack: the
			 * buffer holds garbage and dlen is not trustworthy. */
			break;
		}
		rx_mpdu = hctl->rx_buf;
		hctl->rx_buf = ssv_skb_alloc(MAX_FRAME_SIZE);
		if (hctl->rx_buf == NULL) {
			pr_err("RX buffer allocation failure!\n");
			hctl->rx_buf = rx_mpdu;
			break;
		}
		hctl->rx_pkt++;
		skb_put(rx_mpdu, dlen);
		__skb_queue_tail(&rx_list, rx_mpdu);
		if (HCI_IRQ_STATUS(hctl, &status) < 0) {
			status = 0;
			hctl->rx_last_status_valid = false;
		} else if (!(status & SSV6XXX_INT_RX)) {
			hctl->rx_last_status = status;
			hctl->rx_last_status_valid = true;
		}
	}
	hctl->shi->hci_rx_cb(&rx_list, hctl->shi->rx_cb_args);
	return ret;
}

static void ssv6xxx_hci_rx_work(struct work_struct *work)
{
	struct sk_buff_head rx_list;
	struct sk_buff *rx_mpdu;
	int rx_cnt, ret;
	size_t dlen;
	u32 status;
	ctrl_hci->rx_work_running = 1;
	skb_queue_head_init(&rx_list);
	status = SSV6XXX_INT_RX;
	for (rx_cnt = 0; (status & SSV6XXX_INT_RX) && (rx_cnt < 32); rx_cnt++) {
		/* in: room in rx_buf, out: frame length */
		dlen = MAX_FRAME_SIZE;
		ret = IF_RECV(ctrl_hci, ctrl_hci->rx_buf->data, &dlen);
		if (ret < 0 || dlen <= 0) {
			pr_warn_ratelimited("%s(): IF_RECV() returns %d (dlen=%d)\n",
			       __FUNCTION__, ret, (int)dlen);
			/* Never hand a failed/short read to the stack: the
			 * buffer holds garbage and dlen is not trustworthy. */
			break;
		}
		rx_mpdu = ctrl_hci->rx_buf;
		ctrl_hci->rx_buf = ssv_skb_alloc(MAX_FRAME_SIZE);
		if (ctrl_hci->rx_buf == NULL) {
			pr_err("RX buffer allocation failure!\n");
			ctrl_hci->rx_buf = rx_mpdu;
			break;
		}
		ctrl_hci->rx_pkt++;
		skb_put(rx_mpdu, dlen);
		__skb_queue_tail(&rx_list, rx_mpdu);
		HCI_IRQ_STATUS(ctrl_hci, &status);
	}
	ctrl_hci->shi->hci_rx_cb(&rx_list, ctrl_hci->shi->rx_cb_args);
	ctrl_hci->rx_work_running = 0;
}

static int _isr_do_rx(struct ssv6xxx_hci_ctrl *hctl, u32 isr_status)
{
	int status;
	u32 before = jiffies;

	if (hctl->isr_summary_eable && hctl->prev_rx_isr_jiffes) {
		if (hctl->isr_rx_idle_time) {
			hctl->isr_rx_idle_time +=
			    (jiffies - hctl->prev_rx_isr_jiffes);
			hctl->isr_rx_idle_time = hctl->isr_rx_idle_time >> 1;
		} else {
			hctl->isr_rx_idle_time +=
			    (jiffies - hctl->prev_rx_isr_jiffes);
		}
	}
	status = _do_rx(hctl, isr_status);
	if (hctl->isr_summary_eable) {
		if (hctl->isr_rx_time) {
			hctl->isr_rx_time += (jiffies - before);
			hctl->isr_rx_time = hctl->isr_rx_time >> 1;
		} else {
			hctl->isr_rx_time += (jiffies - before);
		}
		hctl->prev_rx_isr_jiffes = jiffies;
	}
	return status;
}

static int _do_tx(struct ssv6xxx_hci_ctrl *hctl, u32 status)
{
	int q_num;
	int tx_count = 0;
	u32 to_disable_int = 1;
	unsigned long flags;
	struct ssv_hw_txq *hw_txq;
	if ((status & SSV6XXX_INT_RESOURCE_LOW) == 0)
		return 0;
	for (q_num = (SSV_HW_TXQ_NUM - 1); q_num >= 0; q_num--) {
		u32 before = jiffies;
		hw_txq = &hctl->hw_txq[q_num];
		tx_count += ssv6xxx_hci_tx_handler(hw_txq, 999);
		if (hctl->isr_summary_eable) {
			if (hctl->isr_tx_time) {
				hctl->isr_tx_time += (jiffies - before);
				hctl->isr_tx_time = hctl->isr_tx_time >> 1;
			} else {
				hctl->isr_tx_time += (jiffies - before);
			}
		}
	}
	mutex_lock(&hctl->hci_mutex);
	spin_lock_irqsave(&hctl->int_lock, flags);
	for (q_num = (SSV_HW_TXQ_NUM - 1); q_num >= 0; q_num--) {
		hw_txq = &hctl->hw_txq[q_num];
		if (skb_queue_len(&hw_txq->qhead) > 0) {
			to_disable_int = 0;
			break;
		}
	}
	if (to_disable_int) {
		u32 reg_val;
		hctl->int_mask &= ~(SSV6XXX_INT_RESOURCE_LOW | SSV6XXX_INT_TX);
		reg_val = ~hctl->int_mask;
		spin_unlock_irqrestore(&hctl->int_lock, flags);
		HCI_IRQ_SET_MASK(hctl, reg_val);
	} else {
		spin_unlock_irqrestore(&hctl->int_lock, flags);
	}
	mutex_unlock(&hctl->hci_mutex);
	return tx_count;
}

irqreturn_t ssv6xxx_hci_isr(int irq, void *args)
{
	struct ssv6xxx_hci_ctrl *hctl = args;
	u32 status;
	unsigned long flags;
	int ret = IRQ_HANDLED;
	bool dbg_isr_miss = true;
	if (ctrl_hci->isr_summary_eable && ctrl_hci->prev_isr_jiffes) {
		if (ctrl_hci->isr_idle_time) {
			ctrl_hci->isr_idle_time +=
			    (jiffies - ctrl_hci->prev_isr_jiffes);
			ctrl_hci->isr_idle_time = ctrl_hci->isr_idle_time >> 1;
		} else {
			ctrl_hci->isr_idle_time +=
			    (jiffies - ctrl_hci->prev_isr_jiffes);
		}
	}
	bool have_status = false;
	BUG_ON(!args);
	do {
		mutex_lock(&hctl->hci_mutex);
		if (hctl->int_status) {
			u32 regval;
			spin_lock_irqsave(&hctl->int_lock, flags);
			hctl->int_mask |= hctl->int_status;
			hctl->int_status = 0;
			regval = ~ctrl_hci->int_mask;
			smp_mb();
			spin_unlock_irqrestore(&hctl->int_lock, flags);
			HCI_IRQ_SET_MASK(hctl, regval);
		}
		if (have_status) {
			/* fresh value from the end of _do_rx() */
			have_status = false;
			ret = 0;
		} else {
			ret = HCI_IRQ_STATUS(hctl, &status);
		}
		if ((ret < 0) || ((status & hctl->int_mask) == 0)) {
			mutex_unlock(&hctl->hci_mutex);
			ret = IRQ_NONE;
			break;
		}
		spin_lock_irqsave(&hctl->int_lock, flags);
		status &= hctl->int_mask;
		spin_unlock_irqrestore(&hctl->int_lock, flags);
		mutex_unlock(&hctl->hci_mutex);
		ctrl_hci->isr_running = 1;
		hctl->rx_last_status_valid = false;
		if (status & SSV6XXX_INT_RX) {
			ret = _isr_do_rx(hctl, status);
			if (ret < 0) {
				ret = IRQ_NONE;
				break;
			}
			dbg_isr_miss = false;
		}
		if (_do_tx(hctl, status)) {
			dbg_isr_miss = false;
		} else if (hctl->rx_last_status_valid) {
			/* RX drained and TX touched nothing: the status read
			 * at the end of the RX loop is still current. */
			status = hctl->rx_last_status;
			have_status = true;
		} else if ((status & SSV6XXX_INT_RESOURCE_LOW) &&
			   !(status & SSV6XXX_INT_RX) &&
			   (hctl->int_mask & SSV6XXX_INT_RESOURCE_LOW)) {
			/*
			 * Frames are queued but the chip had no free page/ID,
			 * and RESOURCE_LOW stays asserted.  Looping straight
			 * back re-reads the TX resource register thousands of
			 * times per second (one CMD52 + two CMD53 each) and
			 * starves the bus.  Give the air a moment to drain the
			 * chip queue.  This runs from the SDIO IRQ work with the
			 * host released, so sleeping is fine; pending RX is
			 * served first on the next pass.
			 */
			usleep_range(500, 1000);
		}
		ctrl_hci->isr_running = 0;
	} while (1);
	if (ctrl_hci->isr_summary_eable) {
		if (dbg_isr_miss)
			ctrl_hci->isr_miss_cnt++;
		ctrl_hci->prev_isr_jiffes = jiffies;
	}
	return ret;
}

static struct ssv6xxx_hci_ops hci_ops = {
	.hci_start = ssv6xxx_hci_start,
	.hci_stop = ssv6xxx_hci_stop,
	.hci_read_word = ssv6xxx_hci_read_word,
	.hci_write_word = ssv6xxx_hci_write_word,
	.hci_tx = ssv6xxx_hci_enqueue,
	.hci_tx_pause = ssv6xxx_hci_txq_pause,
	.hci_tx_resume = ssv6xxx_hci_txq_resume,
	.hci_txq_flush = ssv6xxx_hci_txq_flush,
	.hci_txq_flush_by_sta = ssv6xxx_hci_txq_flush_by_sta,
	.hci_txq_empty = ssv6xxx_hci_is_txq_empty,
	.hci_load_fw = ssv6xxx_hci_load_fw,
	.hci_pmu_wakeup = ssv6xxx_hci_pmu_wakeup,
	.hci_send_cmd = ssv6xxx_hci_send_cmd,
	.hci_write_sram = ssv6xxx_hci_write_sram,
	.hci_interface_reset = ssv6xxx_hci_interface_reset,
};

int ssv6xxx_hci_deregister(void)
{
	u32 regval;
	pr_debug("%s(): \n", __FUNCTION__);
	if (ctrl_hci->shi == NULL)
		return -1;
	regval = 1;
	ssv6xxx_hci_irq_disable();
	flush_workqueue(ctrl_hci->hci_work_queue);
	destroy_workqueue(ctrl_hci->hci_work_queue);
	ctrl_hci->shi = NULL;
	return 0;
}

EXPORT_SYMBOL(ssv6xxx_hci_deregister);
int ssv6xxx_hci_register(struct ssv6xxx_hci_info *shi)
{
	int i;
	if (shi == NULL || ctrl_hci->shi)
		return -1;
	shi->hci_ops = &hci_ops;
	ctrl_hci->shi = shi;
	ctrl_hci->txq_mask = 0;
	mutex_init(&ctrl_hci->txq_mask_lock);
	mutex_init(&ctrl_hci->hci_mutex);
	spin_lock_init(&ctrl_hci->int_lock);

	for (i = 0; i < SSV_HW_TXQ_NUM; i++) {
		memset(&ctrl_hci->hw_txq[i], 0, sizeof(struct ssv_hw_txq));
		skb_queue_head_init(&ctrl_hci->hw_txq[i].qhead);
		ctrl_hci->hw_txq[i].txq_no = (u32) i;
		ctrl_hci->hw_txq[i].max_qsize = SSV_HW_TXQ_MAX_SIZE;
		ctrl_hci->hw_txq[i].resum_thres = SSV_HW_TXQ_RESUME_THRES;
	}
	ctrl_hci->hci_work_queue =
	    create_singlethread_workqueue("ssv6xxx_hci_wq");
	INIT_WORK(&ctrl_hci->hci_rx_work, ssv6xxx_hci_rx_work);
	INIT_WORK(&ctrl_hci->hci_tx_work, ssv6xxx_hci_tx_work);
	ctrl_hci->int_mask = SSV6XXX_INT_RX | SSV6XXX_INT_RESOURCE_LOW;
	ctrl_hci->int_status = 0;
	HCI_IRQ_SET_MASK(ctrl_hci, 0xFFFFFFFF);
	ssv6xxx_hci_irq_disable();
	HCI_IRQ_REQUEST(ctrl_hci, ssv6xxx_hci_isr);
	return 0;
}

EXPORT_SYMBOL(ssv6xxx_hci_register);
int ssv6xxx_hci_init(void)
{
#ifdef CONFIG_SSV6200_CLI_ENABLE
	extern struct ssv6xxx_hci_ctrl *ssv_dbg_ctrl_hci;
#endif
	ctrl_hci = kzalloc(sizeof(*ctrl_hci), GFP_KERNEL);
	if (ctrl_hci == NULL)
		return -ENOMEM;
	memset((void *)ctrl_hci, 0, sizeof(*ctrl_hci));
	ctrl_hci->rx_buf = ssv_skb_alloc(MAX_FRAME_SIZE);
	if (ctrl_hci->rx_buf == NULL) {
		kfree(ctrl_hci);
		return -ENOMEM;
	}
#ifdef CONFIG_SSV6200_CLI_ENABLE
	ssv_dbg_ctrl_hci = ctrl_hci;
#endif
	return 0;
}

void ssv6xxx_hci_exit(void)
{
#ifdef CONFIG_SSV6200_CLI_ENABLE
	extern struct ssv6xxx_hci_ctrl *ssv_dbg_ctrl_hci;
#endif
	kfree(ctrl_hci);
	ctrl_hci = NULL;
#ifdef CONFIG_SSV6200_CLI_ENABLE
	ssv_dbg_ctrl_hci = NULL;
#endif
}

EXPORT_SYMBOL(ssv6xxx_hci_init);
EXPORT_SYMBOL(ssv6xxx_hci_exit);
