# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_SSV6051) += ssv6051.o
ssv6051-y := sdio.o hw.o mac.o tx.o rx.o rc.o ampdu.o ap.o

ifeq ($(KERNELRELEASE),)
# Out-of-tree build: make [KVER=<kernel version>] [KDIR=<kernel build dir>]
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) CONFIG_SSV6051=m modules

install: all
	install -D -m 644 ssv6051.ko $(DESTDIR)/lib/modules/$(KVER)/updates/ssv6051.ko
	install -D -m 644 ssv6051-sw.bin $(DESTDIR)/lib/firmware/ssv/ssv6051-sw.bin
	[ -n "$(DESTDIR)" ] || depmod -a $(KVER)

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all install clean
endif
