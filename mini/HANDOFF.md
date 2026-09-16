# Driver mínimo SSV6051 (`ssv6051m`): instruções para continuar

Este arquivo é para quem (humano ou agente, ex.: Codex) continuar o
trabalho no driver novo em `mini/`. **Escopo: só o `mini/`.** O driver
legado (resto do repositório) é só referência para ler, não mexer.

## Objetivo

Driver pequeno, limpo, com cara de mainline, para o SSV6051P em SDIO
(TV boxes RK322x / S905W):

- modo estação (cliente) apenas: WPA2-PSK, WPA2-Enterprise/PEAP (a
  criptografia é por software no mac80211, então PEAP/eduroam funciona sem
  nada especial);
- **AP/hotspot ficou fora de propósito** (decisão do dono do projeto);
- funciona em 32 e 64 bits (sem `long` em struct de fio, tudo `__packed`,
  `get/put_unaligned_le*`, buffers DMA-safe para SDIO);
- **sem arquivo `.cfg`**: configuração por parâmetros de módulo ou por
  propriedades de DT;
- estilo do kernel (checkpatch), sem código morto, comentários curtos.

## Estado atual (branch `mini`)

Funciona: probe, upload do firmware, calibração, scan, associação WPA2 em
~4 s, DHCP, HT20 com SGI (MCS0-7), controle de taxa próprio, AMPDU no RX
(sessão de BA no chip) e AMPDU no TX (agregado montado no host, BA
encaminhado pelo firmware, retentativa pelo host, até 3 agregados em voo
por TID), decifragem CCMP unicast no chip (`hw_decrypt=1`), suspend/resume
(só compilado: o rk não tem RTC nem `pm_test`, não dá para testar sem
acesso físico).

Vazão (iperf3 contra o Cudy, rk a poucos cm do TP-Link, mesmas condições):

| driver | up Mbit/s | down Mbit/s |
|---|---|---|
| legado (`ssv6051.ko` + cfg) | 8.0-9.9 | 11.1-12.8 |
| mini (`ssv6051m.ko`) | 8.0-8.8 | 11.5-12.5 |

Os números variam bastante com o horário (canal 2.4 GHz cheio); sempre
compare os dois drivers na mesma janela de tempo.

## Arquivos

| arquivo | conteúdo |
|---|---|
| `sdio.c` | bus SDIO: registradores (CMD52/CMD53), IRQ, upload do firmware, PMU sleep/wake, probe/remove, reboot notifier, parâmetros de módulo e DT |
| `hw.c` | e-fuse (chip id, MAC), init de RF/PHY/MAC, canal, calibração, comandos ao firmware, WSID, BSSID, EDCA, sessão RX BA |
| `mac.c` | `ieee80211_ops` e registro no mac80211 |
| `tx.c` | descritor de TX, orçamento de páginas/ids do chip, thread de TX, filas por AC |
| `rx.c` | leitura de frames/eventos, `rx_status`, despacho de BA/NO_BA |
| `rc.c` | tabela de taxas e controle de taxa (janelas + sondagem) |
| `ampdu.c` | AMPDU TX |
| `ssv6051.h` | structs de fio, estado do driver, protótipos |
| `reg.h`, `aux.h`, `tables.h` | registradores e tabelas de init herdadas do fabricante |

Firmware: `/lib/firmware/ssv6051-sw.bin` (o mesmo do legado).

## Acesso ao hardware

Tudo a partir da VPS (onde está o clone `/root/ssvref/6051` e o worktree
`/root/ssvref/6051-mini`):

- **rk (a TV box com o SSV6051):** `ssh root@10.8.0.12` (WireGuard direto,
  chave já instalada). Kernel `6.18.44-current-rockchip` (Armbian, 32 bits),
  headers instalados, compila no próprio rk.
- **Cudy (roteador, 192.168.1.1):** `ssh root@10.8.0.4`. É o servidor iperf3.
- **TP-Link (AP ao qual o rk se associa, 192.168.1.2):**
  `/root/ssvref/tools/tp.sh <comando>` (passa pelo rk, senha 1234).
  Útil: `iw dev <if> station dump`, estado de agregação no debugfs.

### Restrições importantes

- **Sem acesso físico e sem console serial.** Não dá para tirar da tomada.
  Se o rk travar, só volta se o watchdog reiniciar. Seja conservador:
  nada de mexer em clock SDIO acima de 50 MHz (37.5 MHz quebra no RK322x;
  25 e 50 funcionam), nada de testes que possam travar o kernel sem
  necessidade.
- O WireGuard do rk sai pelo **cabo** (`end0`, 192.168.1.163), então
  derrubar o `wlan0` (192.168.1.181) não corta o acesso. Por isso os testes
  usam `--bind-dev wlan0`; confira com `ip -br a` que o `wlan0` tem IP.
- No boot o rk carrega o **driver legado** (`updates/ssv6051.ko` + cfg).
  O mini é carregado à mão (script abaixo). Não troque o padrão de boot sem
  o ok do dono.
- Não usar `/tmp` compartilhado; use um diretório de trabalho próprio.

## Ciclo de trabalho

Compilar no rk e recarregar (`/root/ssvref/tools/mini-reload.sh`):

```sh
#!/bin/sh
# Compila o mini no rk e recarrega. Uso: mini-reload.sh [params]
MKFLAGS="${MKFLAGS:-}"
cd /root/ssvref/6051-mini && tar -cf - mini | ssh root@10.8.0.12 "MKFLAGS='$MKFLAGS'; rm -rf /root/mini-build && mkdir -p /root/mini-build && tar -x -C /root/mini-build && cd /root/mini-build/mini && make KVER=6.18.44-current-rockchip -j2 $MKFLAGS 2>&1 | grep -E 'error|warning' | grep -v 'compiler differs'
rmmod ssv6051m 2>/dev/null; rmmod ssv6051 2>/dev/null; sleep 1; dmesg -C
insmod ssv6051m.ko tx_gain=14 sdio_clock_hz=50000000 $* || exit 1
sleep 3; systemctl restart netplan-wpa-wlan0
for i in \$(seq 1 20); do s=\$(wpa_cli -p /run/wpa_supplicant -i wlan0 status 2>/dev/null | sed -n 's/^wpa_state=//p'); [ \"\$s\" = COMPLETED ] && break; sleep 2; done
echo \"mini: wpa=\$s em \$((i*2))s\""
```

- Com `dev_dbg` ligado: `MKFLAGS="KCFLAGS=-DDEBUG" mini-reload.sh`
  (o kernel não tem dynamic debug). O debug do AMPDU/RC gera muito log e
  derruba a vazão; meça sempre sem ele.
- Se o `insmod` falhar com "Unknown symbol", faltou `modprobe mac80211`.

Medir (`/root/ssvref/tools/perf.sh [N]`, N rodadas de 10 s up/down):

```sh
for i in $(seq 1 ${1:-2}); do for dir in up down; do
  ssh root@10.8.0.4 'killall iperf3 2>/dev/null; (iperf3 -s -1 -p 5299 >/dev/null 2>&1 &)'; sleep 1
  R=""; [ $dir = down ] && R="-R"
  ssh root@10.8.0.12 "iperf3 -c 192.168.1.1 -p 5299 --bind-dev wlan0 -t 10 -O 2 $R 2>&1 | grep -E ' receiver|error' | awk -v d=$dir '{printf \"%s %s  \", d, \$7}'"
done; done; echo
```

Voltar ao legado para comparar:

```sh
ssh root@10.8.0.12 'rmmod ssv6051m; /root/wifitest.sh /lib/modules/$(uname -r)/updates/ssv6051.ko /lib/firmware/ssv6051-wifi.cfg'
```

Commits: em português, curtos, no branch `mini`, um por mudança testada.
Só dar push quando o dono pedir.

## O que se aprendeu (não redescubra)

SDIO / chip:
- Registradores do function 1 por CMD52; registradores do chip por CMD53
  na porta de registrador; frames pela porta de dados. O tamanho do frame
  de RX são **dois CMD52** (`SDIO_REG_RX_LEN0/1`); CMD53 nesses dá timeout.
- Depois de reboot a quente o chip fica em estado ruim → no probe faz
  sleep do PMU + wake (`ssv_reset_chip`) e o reboot notifier deixa o chip
  dormindo. Sem isso a associação falha no boot.
- O probe sobe a 25 MHz e só depois do firmware rodando passa para
  `sdio_clock_hz` (50 MHz ok).

mac80211 (6.18):
- `rx_status`: usar `encoding`/`enc_flags` (o legado misturava flags antigas
  e marcava frames como MMIC_ERROR/ONLY_MONITOR).
- `IEEE80211_HW_HAS_RATE_CONTROL` exige `.set_rts_threshold`, senão WARN
  em `rate.c`.
- O relógio do chip não é TSF: o timestamp de beacon/probe_resp é trocado
  por `ktime` para o mac80211 não se perder.

Firmware:
- Descritor de TX de 80 bytes (`struct ssv_tx_desc`) com `rc_params[3]`.
- `SSV_EVT_RC_MPDU_REPORT` (2) **acumula a janela** desde o último pedido:
  `ampdu_len` = frames, `ampdu_ack_len` = ACKs, `rates[0].count` =
  transmissões. O RC manda janelas de 16 frames numa taxa e pede relatório
  no último.
- Evento 9 a cada ~5 s = tick do watchdog do firmware (ignorado).
- `SSV_EVT_TXLOOPBK_RESULT` (10) = fim da calibração.

AMPDU:
- RX: sessão de BA programada em `ADR_BA_*`; o chip só aceita **uma**
  sessão por vez.
- TX: delimitador de 4 bytes = LE16 (`len << 4`), CRC, 0x4E; depois MPDU,
  4 bytes de FCS reservados (o chip preenche com `MTX_AMPDU_CRC_AUTO`) e
  padding até múltiplo de 4. `hdr_offset` continua `TXPB_OFFSET` (sem somar
  o delimitador).
- `ADR_RX_FLOW_CTRL` precisa passar pela CPU
  (`MACRX | CPU<<4 | HWHCI<<8`) para o firmware anexar a nota ao BA.
- A nota (`struct ssv_ba_note`: wsid, tried[3], seq[24]) está no **fim do
  frame BA sem cortar o `RX_PINFO_PAD`**. Cortando antes, wsid vira lixo.
- Um agregado por TID em voo; teto de bytes =
  `(HW_TX_PAGES/2) << HW_PAGE_SHIFT` menos reserva (~14 kB, ~9 MPDUs de
  1500), igual ao legado (`ampdu_divider = 2`).
- A thread de TX precisa ser acordada quando chega BA (`ssv_tx_kick`),
  senão só roda a cada 50 ms e a vazão cai para ~1.5 Mbit/s.

## Pendências (ordem sugerida)

Já feito e medido: vários agregados em voo (ganho pequeno no up),
decifragem no chip (~25% menos CPU por Mbit/s no down), suspend/resume.
Testado e descartado: RX STBC (o AP passa a mandar STBC e o down cai ~1/3;
comentário em `mac.c`), agregados maiores que ~14 kB (sem ganho; e o
`tx_buf` tem 16 kB, há trava).

1. **Remover os `dev_dbg` mais ruidosos** de `ampdu.c`/`rx.c` (evento
   desconhecido) ou deixá-los só onde ajudam; conferir com `checkpatch.pl
   --strict -f`.
2. **Testar suspend/resume** numa placa com acesso físico (ou com RTC):
   `rtcwake -m mem -s 20`, depois ver se reassocia.
3. **Power save 802.11**: não implementado de propósito. O legado também
   não tinha (o comando PS do firmware só estaciona o MCU); TV box fica na
   tomada e PS só piora latência/vazão. Se um dia quiser: descobrir se o
   firmware acorda sozinho no beacon/DTIM antes de ligar `SUPPORTS_PS`.
4. **Cifragem no chip para frames não agregados** (gerência, EAPOL,
   pares sem HT): exigiria `set_key` devolver 0 e cifrar agregados no host
   ou no chip; hoje o mac80211 cifra tudo e isso não é gargalo.
5. **Teste de robustez**: 20 recargas seguidas, scan com tráfego,
   roaming/reassociação, `rmmod` com tráfego, AP sumindo, rekey de PTK
   (a chave no chip é trocada em `set_key`); olhar `dmesg` por WARN/leak.
6. **Boot pelo mini** (só com ok do dono): instalar em `updates/`,
   blacklist do legado, 5+ reboots limpos (há `boottest.sh` nas tools).
7. **Preparar para mainline**: binding de DT (`ssv,*`), `Kconfig`,
   MAINTAINERS, firmware em linux-firmware, nome definitivo do módulo.

Fora do escopo: AP/hotspot, P2P, IBSS, 40 MHz (o chip é HT20), monitor.

## Referências

- Driver legado limpo: branch `limpeza` (`/root/ssvref/6051-limpeza`),
  arquivos úteis: `smac/ampdu.c`, `smac/dev.c`, `smac/init.c`,
  `include/ssv6200_common.h`, `hwif/sdio/sdio.c`.
- Datasheet do módulo iTM1020 (usa o SV6051P):
  <https://www.iottech-corp.com/datasheet/iot/iTM1020_Datasheet_V1.6_12052016.pdf>.
  O que interessa: SDIO 2.0 a **50 MHz** (4 e 1 bit); 802.11n MCS0-7 só
  em 20 MHz, GI longo e curto; agregação, RIFS, **STBC** e **Greenfield**
  anunciados; TX típico 18 dBm (b), 14 dBm (g), 13.5 dBm (n); sensibilidade
  MCS7 -70 dBm; `LDO_EN` liga/desliga o chip; POR em 1.3 ms, depois o host
  carrega o firmware (DPLL estabiliza em 100 µs). Não traz registradores.
- <https://pt.scribd.com/document/732242071/5bac8f3815d5b> ("SV6051P WLAN
  Chip Datasheet"): o Scribd não entrega o conteúdo sem login; não foi lido.
