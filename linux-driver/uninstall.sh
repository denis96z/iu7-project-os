#!/bin/bash

[ ! -z "$(lsmod | grep blm)" ] && rmmod blm_driver
rm -vf /lib/modules/*/extra/blm_driver.ko
rm -vf /etc/modprobe.d/blm.conf
depmod -a