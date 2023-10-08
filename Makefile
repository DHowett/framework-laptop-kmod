KDIR ?= /lib/modules/$(shell uname -r)/build
MDIR := $(shell dirname $(realpath $(firstword $(MAKEFILE_LIST))))

.PHONY: all modules modules_install

all: modules

modules modules_install:
	make -C $(KDIR) M=$(MDIR) $@
