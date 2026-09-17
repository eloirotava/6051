# ssv6051: driver Linux para o Wi-Fi SSV6051P (SDIO)

Driver para o mac80211, escrito do zero, para o chip iComm/SSV SSV6051P
usado em TV boxes (RK322x, S905W e similares). Substitui o driver do
fabricante (`ssv6051.ko` antigo + `ssv6051-wifi.cfg`).

- **modos:** cliente (WPA2-PSK, WPA2-Enterprise/PEAP) e hotspot (AP com
  `hostapd`), um de cada vez;
- **rádio:** 802.11b/g/n em 2,4 GHz, HT20 com short GI (até MCS7);
- **agregação:** AMPDU no envio e na recepção;
- **plataformas:** 32 e 64 bits, independente de endianness;
- **configuração:** device tree, sem arquivo `.cfg` nem parâmetros de módulo;
- **estilo:** passa no `checkpatch.pl --strict`;
- **testado em:** kernel 6.18 (Armbian, RK3228).

## Arquivos

| arquivo | conteúdo |
|---|---|
| `Makefile`, `Kconfig` | compilação fora ou dentro da árvore do kernel |
| `sdio.c` | barramento SDIO, firmware, probe, configuração da placa |
| `hw.c` | inicialização do chip, canal, calibração, beacon |
| `mac.c` | interface com o mac80211 |
| `tx.c`, `rx.c` | envio e recepção |
| `rc.c` | controle de taxa |
| `ampdu.c` | agregação no envio |
| `ap.c` | modo hotspot |
| `ssv6051.h`, `reg.h`, `tables.h` | formatos do firmware, registradores e tabelas do chip |
| `ssv6051-sw.bin` | firmware do chip (instalado como `ssv/ssv6051-sw.bin`) |
| `Documentation/.../ssv,ssv6051.yaml` | binding de device tree |

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

O resultado é o `ssv6051.ko`. Para outro kernel ou compilação cruzada:

```sh
make KVER=6.18.44-current-rockchip
make KDIR=/caminho/do/kernel ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-
```

## Instalar

O driver antigo do fabricante tem o mesmo nome de módulo (`ssv6051`).
Remova-o antes, junto com qualquer `blacklist`/`options` que exista para
ele em `/etc/modprobe.d/`:

```sh
sudo find /lib/modules/$(uname -r) -name 'ssv6051*.ko*' -delete
sudo make install        # módulo em updates/, firmware em /lib/firmware/ssv/
```

Para carregar sem reiniciar:

```sh
sudo modprobe -r ssv6051
sudo modprobe ssv6051
```

No boot, o módulo é carregado automaticamente pelo ID SDIO (3030:3030).

## Configuração (device tree)

Sem nada no device tree, o driver usa cristal de 24 MHz, LDO interno e
potência padrão do chip, o que funciona na maioria das boxes. O clock do
SDIO é o que o kernel negocia com o controlador (até 50 MHz); o driver só
o reduz para 25 MHz enquanto inicializa o chip e carrega o firmware. Para
limitá-lo, use a propriedade padrão `max-frequency` no nó do controlador. Para ajustar, descreva a função SDIO como filha do controlador
MMC (binding completo em `Documentation/devicetree/bindings/net/wireless/ssv,ssv6051.yaml`):

| propriedade | valores | padrão |
|---|---|---|
| `ssv,xtal-hz` | 24000000, 26000000, 40000000 | 24000000 |
| `ssv,dcdc` | presente = alimentação por DC-DC | LDO |
| `ssv,tx-gain-level` | 1 (maior potência) a 14 (menor) | padrão do chip |

Exemplo de overlay para RK322x no Armbian (salve como
`/boot/overlay-user/ssv6051.dts`, compile com
`dtc -@ -I dts -O dtb -o ssv6051.dtbo ssv6051.dts`, acrescente
`ssv6051` a `user_overlays=` no `/boot/armbianEnv.txt` e reinicie):

```dts
/dts-v1/;
/plugin/;

/ {
	compatible = "rockchip,rk3228", "rockchip,rk3229";

	fragment@0 {
		target = <&sdio>;	/* confira o rótulo do mmc@30010000 */
		__overlay__ {
			#address-cells = <1>;
			#size-cells = <0>;

			wifi@1 {
				compatible = "ssv,ssv6051";
				reg = <1>;
				ssv,tx-gain-level = <14>;
			};
		};
	};
};
```

## Hotspot

Com `hostapd` (`driver=nl80211`, `hw_mode=g`, `ieee80211n=1`). Cliente e
hotspot ao mesmo tempo não são possíveis: o chip só aceita um endereço
MAC. A internet do hotspot precisa vir de outra interface (cabo, por
exemplo).

## Integração na árvore do kernel

Copie os fontes para `drivers/net/wireless/ssv/ssv6051/`, inclua o
`Kconfig` e o `Makefile` a partir de `drivers/net/wireless/ssv/`, e
acrescente:

- `SDIO_VENDOR_ID_SSV` / `SDIO_DEVICE_ID_SSV_6051` (0x3030) em
  `include/linux/mmc/sdio_ids.h`;
- o prefixo `ssv` (South Silicon Valley Microelectronics) em
  `Documentation/devicetree/bindings/vendor-prefixes.yaml`;
- o binding em `Documentation/devicetree/bindings/net/wireless/`;
- o firmware em `linux-firmware` como `ssv/ssv6051-sw.bin`.

## Limitações conhecidas

- Logo após o boot, o primeiro handshake WPA às vezes estoura o tempo (a
  última mensagem não chega ao AP); o `wpa_supplicant` refaz sozinho.
- Só os frames de gerência e EAPOL têm confirmação real (o driver lê os
  contadores do MAC em volta deles) e os agregados, pelo Block Ack; os
  demais dados são reportados como confirmados.
- Sem power save 802.11, sem HT40 (o chip é só 20 MHz).
- Suspend/resume implementado, mas não testado.
- Partes do código e as tabelas vêm do driver do fabricante, cujos
  cabeçalhos citam GPL versão 3 ou posterior; para o kernel oficial isso
  precisaria ser esclarecido com a iComm.
