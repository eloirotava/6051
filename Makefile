# SPDX-License-Identifier: GPL-2.0-or-later
# Out-of-tree build:  make [KVER=<kernel version>] [KDIR=<kernel build dir>]

ssv6051m-y := sdio.o hw.o mac.o tx.o rx.o rc.o ampdu.o ap.o
obj-m += ssv6051m.o

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

install: all
	install -D -m 644 ssv6051m.ko $(DESTDIR)/lib/modules/$(KVER)/updates/ssv6051m.ko
	install -D -m 644 ssv6051-sw.bin $(DESTDIR)/lib/firmware/ssv6051-sw.bin
	[ -n "$(DESTDIR)" ] || depmod -a $(KVER)

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all install clean
