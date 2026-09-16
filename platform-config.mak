# Opcoes de compilacao do ssv6051.
#
# Os recursos opcionais do codigo do fabricante (debugfs antigo, SmartLink,
# P2P NoA, comandos de fornecedor do Android, WAPI, contadores de debug...)
# foram removidos junto com os #ifdef; o que sobrou abaixo e so o que o
# codigo ainda consulta.

CONFIG_SSV6200_CORE=m

ccflags-y += -Os

# Interface de debug em /sys/kernel/debug/ssv/ssv_cmd (leitura/escrita de
# registradores, taxa fixa, filas...).  So root; comente para remover.
ccflags-y += -DCONFIG_SSV6200_CLI_ENABLE

# SDIO: atraso de saida de dados do chip e tamanho de bloco.
ccflags-y += -DCONFIG_PLATFORM_SDIO_OUTPUT_TIMING=3
ccflags-y += -DCONFIG_PLATFORM_SDIO_BLOCK_SIZE=128
