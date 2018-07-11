ifneq ($(KERNELRELEASE),)

# kbuild part of makefile
obj-m  := tcp_pcc.o
#tcp_pcc-y := tcp_pcc.o

else
# normal makefile

KDIR ?= /lib/modules/`uname -r`/build

default:
	$(MAKE) -C $(KDIR) M=$$PWD

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
endif
