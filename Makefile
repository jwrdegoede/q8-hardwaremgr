obj-m += q8-hardwaremgr.o

KBASE  ?= /lib/modules/`uname -r`
KBUILD ?= $(KBASE)/build
MDEST  ?= $(KBASE)/kernel/drivers/misc

all:
	${MAKE} -C $(KBUILD) M=$(PWD) modules

clean:
	${MAKE} -C $(KBUILD) M=$(PWD) clean

install:
	install -D -m 644 q8-hardwaremgr.ko $(MDEST)
	echo "q8-hardwaremgr" > /etc/modules-load.d/q8-hardwaremgr.conf
