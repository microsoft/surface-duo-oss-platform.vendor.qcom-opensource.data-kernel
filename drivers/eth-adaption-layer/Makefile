KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
obj-m += eth-adaption-layer.o
MY_CFLAGS += -g
ccflags-y += ${MY_CFLAGS}
CC += ${MY_CFLAGS}

eth_adaption-layer-objs :=	\
			eth-adaption-main.o \
			eth-adaption-client.o \
			eth-adaption-server.o

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(shell pwd) modules $(KBUILD_OPTIONS)

debug:
	$(MAKE) -C $(KERNEL_SRC) M=$(shell pwd) modules $(KBUILD_OPTIONS) EXTRA_CFLAGS="$(MY_CFLAGS)"

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(shell pwd) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean
