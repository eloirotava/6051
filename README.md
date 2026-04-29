# SV6051P
for kernel 7.0.0

build with make and kernel headers (and build-essentials)



sudo cp ./ssv6051-wifi.cfg /lib/firmware/

sudo cp ./ssv6051-sw.bin /lib/firmware/

sudo cp ./ssv6051.ko /lib/modules/$(uname -r)/kernel/drivers/net/wireless/



sudo depmod -a

sudo modprobe ssv6051