obj-m := blm_driver.o
KVERSION := $(shell uname -r)
KDIR := /lib/modules/$(KVERSION)/build
PWD := $(shell pwd)
modules modules_install clean:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) $@
install: modules_install
	install -D -m 0644 blm.conf /etc/modprobe.d/blm.conf
	depmod -a
uninstall:
	bash uninstall.sh