# SPDX-License-Identifier: GPL-2.0-or-later
# Out-of-tree build:  make [KVER=<kernel version>]
# In-tree:            obj-$(CONFIG_SSV6051) += ssv6051.o

ssv6051m-y := sdio.o hw.o mac.o tx.o rx.o rc.o
obj-m += ssv6051m.o

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
