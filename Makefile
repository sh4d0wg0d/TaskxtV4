# TaskXT V3 - Linux Task Feature Extractor
# Ported for Linux Kernel 6.18.3+
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or (at
# your option) any later version.

obj-m := taskxt.o
taskxt-objs := main.o disk.o

KVER ?= $(shell uname -r)

PWD := $(shell pwd)

.PHONY: modules modules_install clean help

default: modules
	strip --strip-unneeded taskxt.ko
	cp taskxt.ko taskxt-$(KVER).ko
	@echo "Module built successfully as taskxt-$(KVER).ko"

debug:
	KCFLAGS="-DLIME_DEBUG" $(MAKE) -C /lib/modules/$(KVER)/build M=$(PWD) modules
	strip --strip-unneeded taskxt.ko
	cp taskxt.ko taskxt-$(KVER).ko
	@echo "Debug module built successfully"

modules: main.c disk.c
	$(MAKE) -C /lib/modules/$(KVER)/build M=$(PWD) $@

modules_install: modules
	$(MAKE) -C /lib/modules/$(KVER)/build M=$(PWD) $@

clean:
	rm -f *.o *.mod.c Module.symvers Module.markers modules.order \.*.o.cmd \.*.ko.cmd \.*.o.d *.mod

help:
	@echo "TaskXT V3 - Linux Task Feature Extractor"
	@echo "Build targets:"
	@echo "  make          - Build the module"
	@echo "  make debug    - Build with debug output enabled"
	@echo "  make install  - Install the module (requires root)"
	@echo "  make clean    - Clean build artifacts"
	@echo ""
	@echo "Usage (as root):"
	@echo "  insmod taskxt-\$$(uname -r).ko \"path=/tmp pname=ls srate=1 dura=10000\""

	rm -rf \.tmp_versions

distclean: mrproper
mrproper:	clean
	rm -f *.ko 
