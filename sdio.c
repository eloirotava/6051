// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SSV6051 SDIO transport: register and data ports, interrupt, firmware
 * upload, probe/remove.
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/of.h>
#include <linux/reboot.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/unaligned.h>

#include "ssv6051.h"

#ifndef SDIO_VENDOR_ID_SSV
#define SDIO_VENDOR_ID_SSV		0x3030
#define SDIO_DEVICE_ID_SSV_6051		0x3030
#endif

#define FW_BLOCK_SIZE		0x8000
#define FW_CHECKSUM_BLOCK	1024
#define FW_CHECKSUM_INIT	0x12345678
#define FW_STATUS_MASK		0x00ff0000
#define IO_BUF_SIZE		16

/*
 * Register access goes through the "register port": write the address
 * (and value) with CMD53, read the value back with CMD53.  The MMC core
 * needs DMA-safe buffers, hence the per-device scratch buffer; every
 * user holds the SDIO host.
 */
int ssv_reg_read(struct ssv_dev *sd, u32 addr, u32 *val)
{
	struct sdio_func *func = sd->func;
	int ret;

	sdio_claim_host(func);
	put_unaligned_le32(addr, sd->io_buf);
	ret = sdio_memcpy_toio(func, sd->reg_port, sd->io_buf, 4);
	if (!ret)
		ret = sdio_memcpy_fromio(func, sd->io_buf, sd->reg_port, 4);
	sdio_release_host(func);
	if (ret) {
		dev_err_ratelimited(sd->dev, "read 0x%08x failed: %d\n", addr, ret);
		*val = 0xffffffff;
		return ret;
	}
	*val = get_unaligned_le32(sd->io_buf);
	return 0;
}

int ssv_reg_write(struct ssv_dev *sd, u32 addr, u32 val)
{
	struct sdio_func *func = sd->func;
	int ret;

	sdio_claim_host(func);
	put_unaligned_le32(addr, sd->io_buf);
	put_unaligned_le32(val, sd->io_buf + 4);
	ret = sdio_memcpy_toio(func, sd->reg_port, sd->io_buf, 8);
	sdio_release_host(func);
	if (ret)
		dev_err_ratelimited(sd->dev, "write 0x%08x failed: %d\n", addr, ret);
	return ret;
}

int ssv_reg_set_bits(struct ssv_dev *sd, u32 addr, u32 set, u32 mask)
{
	u32 val;
	int ret;

	ret = ssv_reg_read(sd, addr, &val);
	if (ret)
		return ret;
	return ssv_reg_write(sd, addr, (val & ~mask) | (set & mask));
}

/*
 * Frames and host commands.  @buf must be DMA-safe and padded to the
 * SDIO block alignment (see sdio_align_size()).
 */
int ssv_write_data(struct ssv_dev *sd, const u8 *buf, size_t len)
{
	struct sdio_func *func = sd->func;
	int ret;

	sdio_claim_host(func);
	ret = sdio_memcpy_toio(func, sd->data_port, (void *)buf,
			       sdio_align_size(func, len));
	sdio_release_host(func);
	if (ret)
		dev_err_ratelimited(sd->dev, "data write (%zu) failed: %d\n", len, ret);
	return ret;
}

int ssv_irq_mask(struct ssv_dev *sd, u8 mask)
{
	int ret;

	sdio_claim_host(sd->func);
	sdio_writeb(sd->func, mask, SDIO_REG_INT_MASK, &ret);
	sdio_release_host(sd->func);
	return ret;
}

static void ssv_sdio_irq(struct sdio_func *func)
{
	struct ssv_dev *sd = sdio_get_drvdata(func);

	/*
	 * The MMC core calls us with the host claimed and the RX path claims
	 * it again (nested claims by the same task are fine).  Releasing it
	 * here instead would let sdio_release_irq() take the host and wait
	 * for this thread while the thread waits for the host.
	 */
	if (sd && sd->started)
		ssv_rx_irq(sd);
}

int ssv_irq_enable(struct ssv_dev *sd)
{
	int ret;

	sdio_claim_host(sd->func);
	ret = sdio_claim_irq(sd->func, ssv_sdio_irq);
	sdio_release_host(sd->func);
	if (ret)
		return ret;
	return ssv_irq_mask(sd, (u8)~SSV_INT_RX);
}

void ssv_irq_disable(struct ssv_dev *sd)
{
	ssv_irq_mask(sd, 0xff);
	sdio_claim_host(sd->func);
	sdio_release_irq(sd->func);
	sdio_release_host(sd->func);
}

void ssv_set_bus_clock(struct ssv_dev *sd, u32 hz)
{
	struct mmc_host *host = sd->func->card->host;

	/*
	 * Until the firmware has set up the chip PLL, the chip cannot keep
	 * up with a high-speed bus (reads come back corrupted), so bring-up
	 * runs at 25 MHz and the clock the MMC core negotiated is restored
	 * afterwards.  The core has no interface for this, hence set_ios.
	 */
	hz = clamp(hz, host->f_min, host->f_max);
	if (hz == host->ios.clock)
		return;
	sdio_claim_host(sd->func);
	host->ios.clock = hz;
	host->ops->set_ios(host, &host->ios);
	sdio_release_host(sd->func);
	msleep(20);
}

/* Slow bus for chip bring-up, never faster than what the core negotiated */
static void ssv_bus_slow(struct ssv_dev *sd)
{
	ssv_set_bus_clock(sd, min(sd->bus_clock, SDIO_CLOCK_INIT));
}

static int ssv_write_sram(struct ssv_dev *sd, u32 addr, const u8 *data, u32 len)
{
	struct sdio_func *func = sd->func;
	int ret;

	ret = ssv_reg_write(sd, 0xc0000860, addr);
	if (ret)
		return ret;
	sdio_claim_host(func);
	sdio_writeb(func, 0x2, SDIO_REG_FN1_STATUS, &ret);
	if (!ret)
		ret = sdio_memcpy_toio(func, sd->data_port, (void *)data, len);
	if (!ret)
		sdio_writeb(func, 0, SDIO_REG_FN1_STATUS, &ret);
	sdio_release_host(func);
	return ret;
}

static int ssv_upload_firmware(struct ssv_dev *sd, const struct firmware *fw)
{
	u32 checksum = FW_CHECKSUM_INIT, fw_checksum, clk_en, blocks;
	u32 sram = 0, pos = 0;
	u8 *buf;
	int ret;

	buf = kmalloc(FW_BLOCK_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = ssv_reg_write(sd, ADR_BRG_SW_RST, 0);
	if (!ret)
		ret = ssv_reg_write(sd, ADR_BOOT, 1);
	if (!ret)
		ret = ssv_reg_read(sd, ADR_PLATFORM_CLOCK_ENABLE, &clk_en);
	if (!ret)
		ret = ssv_reg_write(sd, ADR_PLATFORM_CLOCK_ENABLE, clk_en | BIT(2));
	if (ret)
		goto out;

	while (pos < fw->size) {
		u32 chunk = min_t(u32, fw->size - pos, FW_BLOCK_SIZE);
		u32 i;

		memset(buf, 0xa5, FW_BLOCK_SIZE);
		memcpy(buf, fw->data + pos, chunk);
		pos += chunk;
		/* the chip checksums whole 1 KiB blocks */
		chunk = round_up(chunk, FW_CHECKSUM_BLOCK);
		ret = ssv_write_sram(sd, sram, buf, chunk);
		if (ret)
			goto out;
		sram += chunk;
		for (i = 0; i < chunk; i += 4)
			checksum += get_unaligned_le32(buf + i);
	}

	checksum = ((checksum >> 24) + (checksum >> 16) + (checksum >> 8) +
		    checksum) & 0xff;
	checksum <<= 16;

	blocks = DIV_ROUND_UP(sram, FW_CHECKSUM_BLOCK);
	ret = ssv_reg_write(sd, ADR_TX_SEG, blocks << 16);
	if (!ret)
		ret = ssv_reg_write(sd, ADR_BRG_SW_RST, 1);	/* start the MCU */
	if (ret)
		goto out;
	msleep(50);

	ret = ssv_reg_read(sd, ADR_TX_SEG, &fw_checksum);
	if (ret)
		goto out;
	fw_checksum &= FW_STATUS_MASK;
	if (fw_checksum != checksum) {
		dev_err(sd->dev, "firmware checksum mismatch (0x%x != 0x%x)\n",
			fw_checksum, checksum);
		ret = -EIO;
		goto out;
	}
	ret = ssv_reg_write(sd, ADR_TX_SEG, ~checksum & FW_STATUS_MASK);
	msleep(50);
out:
	kfree(buf);
	return ret;
}

int ssv_load_firmware(struct ssv_dev *sd)
{
	const struct firmware *fw;
	int ret;

	ret = request_firmware(&fw, SSV_FIRMWARE, sd->dev);
	if (ret) {
		dev_err(sd->dev, "cannot load %s: %d\n", SSV_FIRMWARE, ret);
		return ret;
	}
	ssv_bus_slow(sd);
	ret = ssv_upload_firmware(sd, fw);
	release_firmware(fw);
	if (ret)
		return ret;
	ssv_set_bus_clock(sd, sd->bus_clock);
	return 0;
}

static void ssv_pmu_wakeup(struct ssv_dev *sd)
{
	int ret;

	sdio_claim_host(sd->func);
	sdio_writeb(sd->func, 1, SDIO_REG_PMU_WAKEUP, &ret);
	mdelay(10);
	sdio_writeb(sd->func, 0, SDIO_REG_PMU_WAKEUP, &ret);
	sdio_release_host(sd->func);
}

/* Firmware "power save" command: parks the MCU until the next wakeup. */
static void ssv_pmu_sleep(struct ssv_dev *sd)
{
	struct ssv_host_hdr *cmd;
	size_t len = sizeof(*cmd);

	cmd = kzalloc(sdio_align_size(sd->func, len), GFP_KERNEL);
	if (!cmd)
		return;
	ssv_reg_write(sd, ADR_RX_FLOW_MNG, M_ENG_MACRX | (M_ENG_TRASH_CAN << 4));
	ssv_reg_write(sd, ADR_RX_FLOW_DATA, M_ENG_MACRX | (M_ENG_TRASH_CAN << 4));
	ssv_reg_write(sd, ADR_RX_FLOW_CTRL, M_ENG_MACRX | (M_ENG_TRASH_CAN << 4));
	le32p_replace_bits(&cmd->w0, len, HDR0_LEN);
	le32p_replace_bits(&cmd->w0, HOST_CMD, HDR0_C_TYPE);
	le32p_replace_bits(&cmd->w0, SSV_CMD_PS, HDR0_ID);
	ssv_write_data(sd, (u8 *)cmd, len);
	kfree(cmd);
}

/*
 * After a warm reboot the chip may still run the previous firmware and an
 * upload over it often leaves it unable to ACK.  Replay what a module
 * reload does (sleep command, then the wakeup pulse) so every probe
 * starts from the same state; on a cold chip the command is harmless.
 */
static void ssv_reset_chip(struct ssv_dev *sd)
{
	ssv_pmu_wakeup(sd);
	ssv_pmu_sleep(sd);
	msleep(50);
	ssv_pmu_wakeup(sd);
	usleep_range(10000, 20000);
}

static int ssv_sdio_init(struct ssv_dev *sd)
{
	struct sdio_func *func = sd->func;
	u32 data = 0, reg = 0;
	int ret, i;

	sdio_claim_host(func);
	ret = sdio_enable_func(func);
	if (ret)
		goto out;
	for (i = 0; i < 3; i++) {
		data |= sdio_readb(func, SDIO_REG_DATA_PORT0 + i, &ret) << (8 * i);
		if (ret)
			goto out;
		reg |= sdio_readb(func, SDIO_REG_REG_PORT0 + i, &ret) << (8 * i);
		if (ret)
			goto out;
	}
	sd->data_port = data;
	sd->reg_port = reg;
	ret = sdio_set_block_size(func, SDIO_BLOCK_SIZE);
	if (ret)
		goto out;
	sdio_writeb(func, SDIO_OUTPUT_TIMING, SDIO_REG_OUTPUT_TIMING, &ret);
	if (ret)
		goto out;
	sdio_writeb(func, 0, SDIO_REG_FN1_STATUS, &ret);
out:
	sdio_release_host(func);
	return ret;
}

/*
 * Bring the chip back to its probe-time state (bus, reset, RF setup); the
 * next ssv_hw_start() loads the firmware.
 */
static int ssv_chip_reinit(struct ssv_dev *sd)
{
	int ret;

	ssv_bus_slow(sd);
	ret = ssv_sdio_init(sd);
	if (ret)
		return ret;
	ssv_irq_mask(sd, 0xff);
	ssv_reset_chip(sd);
	return ssv_hw_probe(sd);
}

/*
 * Board description from the SDIO function's device tree node (see
 * Documentation/devicetree/bindings/net/wireless/ssv,ssv6051.yaml).
 */
static void ssv_read_board_config(struct ssv_dev *sd)
{
	struct device_node *np = sd->dev->of_node;
	u32 val;

	sd->xtal = SSV_XTAL_24M;
	sd->ldo = true;
	sd->tx_gain_b = 0;
	sd->tx_gain_gn = 0;
	if (!np)
		return;

	if (!of_property_read_u32(np, "ssv,xtal-hz", &val)) {
		switch (val) {
		case 24000000:
			sd->xtal = SSV_XTAL_24M;
			break;
		case 26000000:
			sd->xtal = SSV_XTAL_26M;
			break;
		case 40000000:
			sd->xtal = SSV_XTAL_40M;
			break;
		default:
			dev_warn(sd->dev, "unsupported crystal %u Hz, using 24 MHz\n", val);
		}
	}
	if (of_property_read_bool(np, "ssv,dcdc"))
		sd->ldo = false;
	if (!of_property_read_u32(np, "ssv,tx-gain-level", &val)) {
		sd->tx_gain_b = val;
		sd->tx_gain_gn = val;
	}
}

static struct sdio_func *ssv_reboot_func;

static int ssv_reboot_notify(struct notifier_block *nb, unsigned long event,
			     void *unused)
{
	struct sdio_func *func = ssv_reboot_func;
	struct ssv_dev *sd = func ? sdio_get_drvdata(func) : NULL;

	if (sd) {
		ssv_irq_mask(sd, 0xff);
		ssv_pmu_sleep(sd);
	}
	return NOTIFY_DONE;
}

static struct notifier_block ssv_reboot_nb = {
	.notifier_call = ssv_reboot_notify,
};

static int ssv_sdio_probe(struct sdio_func *func, const struct sdio_device_id *id)
{
	struct ssv_dev *sd;
	int ret;

	if (func->num != 1)
		return -ENODEV;

	sd = ssv_mac_alloc(&func->dev);
	if (!sd)
		return -ENOMEM;
	sd->func = func;
	sd->io_buf = devm_kzalloc(&func->dev, IO_BUF_SIZE, GFP_KERNEL);
	if (!sd->io_buf) {
		ret = -ENOMEM;
		goto err_free;
	}
	sdio_set_drvdata(func, sd);
	ssv_read_board_config(sd);

	func->card->quirks |= MMC_QUIRK_LENIENT_FN0 | MMC_QUIRK_BLKSZ_FOR_BYTE_MODE;
	sd->bus_clock = func->card->host->ios.clock;
	ssv_bus_slow(sd);
	ret = ssv_sdio_init(sd);
	if (ret) {
		dev_err(&func->dev, "SDIO init failed: %d\n", ret);
		goto err_free;
	}
	ssv_irq_mask(sd, 0xff);
	ssv_reset_chip(sd);

	ret = ssv_hw_probe(sd);
	if (ret)
		goto err_disable;
	ret = ssv_mac_register(sd);
	if (ret)
		goto err_disable;

	ssv_reboot_func = func;
	return 0;

err_disable:
	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);
err_free:
	sdio_set_drvdata(func, NULL);
	ssv_mac_free(sd);
	return ret;
}

static void ssv_sdio_remove(struct sdio_func *func)
{
	struct ssv_dev *sd = sdio_get_drvdata(func);

	ssv_reboot_func = NULL;
	if (!sd)
		return;
	ssv_mac_unregister(sd);
	ssv_irq_mask(sd, 0xff);
	ssv_pmu_sleep(sd);
	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);
	sdio_set_drvdata(func, NULL);
	ssv_mac_free(sd);
}

/*
 * System sleep.  mac80211 stops the device first (the wiphy is our child);
 * the card may lose power, so resume redoes the probe-time bring-up and
 * mac80211's restart reloads the firmware.
 */
static int ssv_sdio_suspend(struct device *dev)
{
	struct ssv_dev *sd = sdio_get_drvdata(dev_to_sdio_func(dev));

	if (!sd)
		return 0;
	ssv_irq_mask(sd, 0xff);
	ssv_pmu_sleep(sd);
	return 0;
}

static int ssv_sdio_resume(struct device *dev)
{
	struct ssv_dev *sd = sdio_get_drvdata(dev_to_sdio_func(dev));

	if (!sd)
		return 0;
	return ssv_chip_reinit(sd);
}

static DEFINE_SIMPLE_DEV_PM_OPS(ssv_sdio_pm, ssv_sdio_suspend, ssv_sdio_resume);

static const struct sdio_device_id ssv_sdio_ids[] = {
	{ SDIO_DEVICE(SDIO_VENDOR_ID_SSV, SDIO_DEVICE_ID_SSV_6051) },
	{ }
};
MODULE_DEVICE_TABLE(sdio, ssv_sdio_ids);

static struct sdio_driver ssv_sdio_driver = {
	.name = "ssv6051",
	.id_table = ssv_sdio_ids,
	.probe = ssv_sdio_probe,
	.remove = ssv_sdio_remove,
	.drv = {
		.pm = pm_sleep_ptr(&ssv_sdio_pm),
	},
};

static int __init ssv_init(void)
{
	int ret;

	register_reboot_notifier(&ssv_reboot_nb);
	ret = sdio_register_driver(&ssv_sdio_driver);
	if (ret)
		unregister_reboot_notifier(&ssv_reboot_nb);
	return ret;
}

static void __exit ssv_exit(void)
{
	unregister_reboot_notifier(&ssv_reboot_nb);
	sdio_unregister_driver(&ssv_sdio_driver);
}

module_init(ssv_init);
module_exit(ssv_exit);

MODULE_DESCRIPTION("South Silicon Valley SSV6051 SDIO 802.11n driver");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(SSV_FIRMWARE);
