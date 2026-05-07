# SSV6051P (and SSV6030P) WiFi Driver

```bash
sudo apt-get update
sudo apt-get install -y build-essential linux-headers-$(uname -r) git
```

# Compilation

```bash
git repoo
make 
```

## 📥 Installation

```bash
sudo cp ./ssv6051-wifi.cfg /lib/firmware/
sudo cp ./ssv6051-sw.bin /lib/firmware/
sudo cp ./ssv6051.ko /lib/modules/$(uname -r)/kernel/drivers/net/wireless/
sudo depmod -a
sudo modprobe ssv6051
```

