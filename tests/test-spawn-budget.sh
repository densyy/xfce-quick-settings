#!/bin/sh
# Budget de spawns: garante que o fluxo Wi-Fi típico não exploda em processos.
# Colhe o argv.log que o próprio test-wifi-security escreve em /tmp
# (o teste sobrescreve QS_FAKE_LOG com tmp próprio — por isso não usamos
# um log fixo aqui). Sem display os E2E dão skip e o budget passa
# vacuamente (documentado).
set -eu
cd "$(dirname "$0")/.."

rm -rf /tmp/qs-test-wifisec-*
./tests/test-wifi-security > /dev/null 2>&1 || {
  echo "spawn-budget: test-wifi-security falhou (ver acima)"
  exit 1
}

LOG=$(ls -t /tmp/qs-test-wifisec-*/argv.log 2>/dev/null | head -1 || true)
if [ -z "${LOG:-}" ]; then
  echo "spawn-budget: FALHOU — argv.log não encontrado"
  exit 1
fi
NMCLI_N=$(grep -c 'nmcli' "$LOG" || true)
TOTAL_N=$(wc -l < "$LOG" | tr -d ' ')
echo "spawn-budget: nmcli=$NMCLI_N total=$TOTAL_N (teto 40)"
if [ "$TOTAL_N" -gt 40 ]; then
  echo "spawn-budget: FALHOU — teto estourado"
  exit 1
fi
echo "spawn-budget: OK"
