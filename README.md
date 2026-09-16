# SSV6051P (and SSV6030P) WiFi Driver

```bash
sudo apt-get update
sudo apt-get install -y build-essential linux-headers-$(uname -r) git
```

# Compilation

```bash
git clone https://github.com/eloirotava/6051.git
cd 6051
make
```

Em placas com 1 GB de RAM, prefira `make -j2`.
Para compilar para outro kernel instalado: `make KVERS_UNAME=<versao>`.

## 📥 Installation

```bash
sudo cp ./ssv6051-wifi.cfg /lib/firmware/
sudo cp ./ssv6051-sw.bin /lib/firmware/
sudo cp ./ssv6051.ko /lib/modules/$(uname -r)/kernel/drivers/net/wireless/
sudo depmod -a
sudo modprobe ssv6051
```

## ⚙️ Configuração

### `/lib/firmware/ssv6051-wifi.cfg`

Lido a cada carga do módulo. Chaves mais relevantes:

| Chave | Padrão no arquivo | Observação |
| --- | --- | --- |
| `sdio_clock_hz` | `50000000` | Clock SDIO após subir o firmware (que sempre sobe a 25 MHz). Sem a chave: 25 MHz. No RK322x, 50 MHz funciona e 37,5 MHz gera erros de CRC (`-84`). |
| `hw_cap_ht` | `on` | 802.11n, MCS 0-7 em 20 MHz. |
| `hw_cap_ampdu_rx` / `hw_cap_ampdu_tx` | `on` | Agregação. O TX agrega no host (um agregado por escrita SDIO). |
| `wifi_tx_gain_level_b` / `_gn` | `14` | Potência de TX: 1 (maior) a 14 (menor); 0 = padrão do chip. |
| `volt_regulator` | `1` | 0 = DCDC, 1 = LDO. |
| `xtal_clock` | `24` | Cristal do módulo (24, 26 ou 40). |

### Parâmetros do módulo

Por exemplo em `/etc/modprobe.d/ssv6051.conf`:

```
options ssv6051 hw_crypto=1
```

| Parâmetro | Padrão | Observação |
| --- | --- | --- |
| `hw_crypto` | `0` | `1` usa o chip para WPA. Com par HT + AMPDU, o chip só decripta e o mac80211 encripta. Reduz CPU na descida (~23% → ~15% num RK3229). |
| `sdio_clock_hz` | `0` | Sobrepõe a chave do cfg (útil para testes). |
| `ssv_initmac` | — | MAC fixo, se o e-fuse não tiver um válido. |

## Resultados (RK3229, kernel 6.18, AP a centímetros)

iperf3 de 60 s contra o roteador:

| Versão | Subida | Descida |
| --- | --- | --- |
| port original (HT off, 25 MHz) | 4,2 Mbit/s | 5,0 Mbit/s |
| + HT/AMPDU | 8,4 Mbit/s | 10,2 Mbit/s |
| + SDIO 50 MHz | 9,4 Mbit/s | 12,8 Mbit/s |
