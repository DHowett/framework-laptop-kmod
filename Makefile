ifneq ($(KERNELRELEASE),)
# kbuild part of makefile
obj-m  := framework_laptop.o

else
# normal makefile
KDIR ?= /lib/modules/`uname -r`/build

modules:

%:
	$(MAKE) -C $(KDIR) M=$$PWD $@

endif

