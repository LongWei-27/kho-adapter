# SPDX-License-Identifier: GPL-2.0
#
# External module build for kho-adapter.
#
# The kernel tree must be configured with CONFIG_KEXEC_HANDOVER=y and it must
# contain the exported walk_page_range() symbol (see patches/).

obj-m += kho_adapter.o
ccflags-y += -I$(src)

KDIR ?= /home/lw/opensource/linux

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	$(MAKE) -C tests clean

test: all
	$(MAKE) -C tests

.PHONY: all modules_install clean test

