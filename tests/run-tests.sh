#!/bin/sh
# Compila e roda a suite de testes do quick-settings. Rode da raiz do repo:
#   sh tests/run-tests.sh
# (O alvo `make test` chama este script.)
set -eu
cd "$(dirname "$0")/.."

PKGS="gtk+-3.0 libxfce4panel-2.0 libxfce4ui-2 libxfce4util-1.0 sqlite3"
CFLAGS_T="-Wall -Wextra -Wconversion -g -std=gnu11 -Isrc -Itests $(pkg-config --cflags $PKGS)"
LIBS_T="$(pkg-config --libs $PKGS) -lsqlite3"

pass=0
fail=0
# shellcheck disable=SC2086
for t in tests/test-*.c; do
  name=$(basename "$t" .c)
  echo "== compila+roda $name =="
  if cc $CFLAGS_T -o "tests/$name" "$t" src/*.c $LIBS_T \
    && "./tests/$name"; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    echo "FALHOU: $name"
  fi
done
echo "== binários ok: $pass, com falha: $fail =="
test "$fail" -eq 0
