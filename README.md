# ssv6051m: driver Linux para o Wi-Fi SSV6051P (SDIO)

Driver mínimo, escrito do zero para o mac80211, para o chip iComm/SSV
SSV6051P usado em TV boxes (RK322x, S905W e similares). Substitui o driver
do fabricante (`ssv6051.ko` + `ssv6051-wifi.cfg`).

- **modos:** cliente (WPA2-PSK, WPA2-Enterprise/PEAP) e hotspot (AP com
  `hostapd`), um de cada vez;
- **rádio:** 802.11b/g/n em 2,4 GHz, HT20 com short GI (até MCS7);
- **agregação:** AMPDU no envio e na recepção;
- **plataformas:** 32 e 64 bits, sem arquivo `.cfg`;
- **testado em:** kernel 6.18 (Armbian, RK3228).

## Arquivos

| arquivo | conteúdo |
|---|---|
| `Makefile` | compilação fora da árvore do kernel e instalação |
| `sdio.c` | barramento SDIO, firmware, probe, parâmetros do módulo |
| `hw.c` | inicialização do chip, canal, calibração, beacon |
| `mac.c` | interface com o mac80211 |
| `tx.c`, `rx.c` | envio e recepção |
| `rc.c` | controle de taxa |
| `ampdu.c` | agregação no envio |
| `ap.c` | modo hotspot |
| `ssv6051.h`, `reg.h`, `aux.h`, `tables.h` | definições e tabelas do chip |
| `ssv6051-sw.bin` | firmware do chip (vai para `/lib/firmware`) |

## Compilar

Você precisa dos headers do kernel em execução e das ferramentas de
compilação. No Debian/Armbian:

```sh
sudo apt install build-essential linux-headers-$(uname -r)
```

No Armbian, os headers vêm no pacote `linux-headers-<branch>-<família>`,
por exemplo `linux-headers-current-rockchip`.

Depois, na pasta do driver:

```sh
make
```

O resultado é o `ssv6051m.ko`. Para outro kernel ou compilação cruzada:

```sh
make KVER=6.18.44-current-rockchip
make KDIR=/caminho/do/kernel ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-
```

## Instalar

```sh
sudo make install        # módulo em /lib/modules/<kver>/updates, firmware em /lib/firmware
```

Se o driver antigo do fabricante estiver instalado, bloqueie-o e configure
o novo, por exemplo em `/etc/modprobe.d/ssv6051.conf`:

```
blacklist ssv6051
options ssv6051m tx_gain=14 sdio_clock_hz=50000000 hw_decrypt=0
```

Para carregar sem reiniciar:

```sh
sudo modprobe -r ssv6051 ssv6051m
sudo modprobe ssv6051m
```

No boot, o módulo é carregado automaticamente pelo ID SDIO (3030:3030).

## Parâmetros

| parâmetro | padrão | significado | propriedade de DT |
|---|---|---|---|
| `xtal_mhz` | 24 | cristal: 24, 26 ou 40 | `ssv,xtal-mhz` |
| `regulator` | LDO | 0 = DCDC, 1 = LDO | `ssv,dcdc` |
| `tx_gain` | 0 | potência 1 (máx) a 14 (mín); 0 = padrão do chip | `ssv,tx-gain-level` |
| `sdio_clock_hz` | 25000000 | clock SDIO após o firmware (50000000 funciona no RK322x; 37500000 não) | `ssv,sdio-clock-hz` |
| `hw_decrypt` | 1 | o chip decifra o CCMP recebido (menos CPU); com 0, a primeira associação após reboot falhou menos nos testes | — |

## Hotspot

Com `hostapd` (`driver=nl80211`, `hw_mode=g`, `ieee80211n=1`). Cliente e
hotspot ao mesmo tempo não são possíveis: o chip só aceita um endereço
MAC. A internet do hotspot precisa vir de outra interface (cabo, por
exemplo).

## Limitações conhecidas

- Após alguns reboots a quente, a primeira associação pode falhar; o
  driver detecta e reinicia o chip sozinho (a conexão sai 15-30 s depois).
- Sem power save 802.11, sem HT40 (o chip é só 20 MHz).
- Suspend/resume implementado, mas não testado.
