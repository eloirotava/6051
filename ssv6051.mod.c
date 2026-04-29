#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

KSYMTAB_DATA(ssv_initmac, "", "");
KSYMTAB_DATA(ssv_devicetype, "", "");
KSYMTAB_FUNC(ssvdevice_init, "", "");
KSYMTAB_FUNC(ssvdevice_exit, "", "");
KSYMTAB_DATA(ssv6xxx_ifdebug_info, "", "");
KSYMTAB_DATA(ssv_cfg, "", "");
KSYMTAB_DATA(cfg_cmds, "", "");
KSYMTAB_DATA(ssv_dbg_phy_table, "", "");
KSYMTAB_DATA(ssv_dbg_phy_len, "", "");
KSYMTAB_DATA(ssv_dbg_rf_table, "", "");
KSYMTAB_DATA(ssv_dbg_rf_len, "", "");
KSYMTAB_DATA(ssv_dbg_sc, "", "");
KSYMTAB_DATA(ssv_dbg_ctrl_hci, "", "");
KSYMTAB_FUNC(ssv6xxx_hci_deregister, "", "");
KSYMTAB_FUNC(ssv6xxx_hci_register, "", "");
KSYMTAB_FUNC(ssv6xxx_hci_init, "", "");
KSYMTAB_FUNC(ssv6xxx_hci_exit, "", "");
KSYMTAB_FUNC(ssv6xxx_dev_probe, "", "");
KSYMTAB_FUNC(ssv6xxx_dev_remove, "", "");
KSYMTAB_FUNC(ssv6xxx_init, "", "");
KSYMTAB_FUNC(ssv6xxx_exit, "", "");
KSYMTAB_DATA(sdio_sr_bhvr, "", "");
KSYMTAB_FUNC(ssv6xxx_get_dev_status, "", "");
KSYMTAB_DATA(ssv6xxx_sdio_driver, "", "");
KSYMTAB_FUNC(ssv6xxx_sdio_init, "", "");
KSYMTAB_FUNC(ssv6xxx_sdio_exit, "", "");

MODULE_INFO(depends, "mac80211,cfg80211");

MODULE_ALIAS("platform:ssv6200");
MODULE_ALIAS("sdio:c*v3030d3030*");
