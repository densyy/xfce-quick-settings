#!/bin/sh
# Roda TODA a suite de testes compilada com sanitizers (ASan+LSan+UBSan).
# Sem valgrind nesta máquina (glibc CachyOS quebra o memcheck) — os
# sanitizers do clang/gcc cumprem o papel de detector de memória/UB.
#
# Uso: sh tests/run-sanitizers.sh
# Requer: gcc com -fsanitize (presente). Roda do diretório que for
# (resolve a raiz do repo via dirname).
set -eu
cd "$(dirname "$0")/.."

PKGS="gtk+-3.0 libxfce4panel-2.0 libxfce4ui-2 libxfce4util-1.0 sqlite3"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
CFLAGS_S="-Wall -Wextra -Wconversion -Isrc -Itests -g -std=gnu11 ${SAN} $(pkg-config --cflags ${PKGS})"
LIBS_S="$(pkg-config --libs ${PKGS}) -lsqlite3 ${SAN}"

pass=0
fail=0
failed=""
for t in tests/test-*.c; do
  case "$t" in
    *test-perf.c) continue ;;  # timing sob sanitizer não é significativo
  esac
  name=$(basename "$t" .c)
  bin="tests/${name}-san"
  echo "== sanitizers: ${name} =="
  # shellcheck disable=SC2086
  if cc ${CFLAGS_S} -o "$bin" "$t" src/*.c ${LIBS_S} > "/tmp/san-${name}.log" 2>&1; then
    # LSan: bins que chamam gtk_init têm roots one-time (type-system/GIO)
  # reportados no exit mesmo com todos os testes verdes — desliga o
  # leak-check SÓ neles (ASan+UBSan continuam ativos em todos).
  leakcheck=detect_leaks=1
  case "$name" in
    test-cache|test-lifecycle|test-wifi-security|test-e2e) leakcheck=detect_leaks=0 ;;
  esac
  if env ASAN_OPTIONS=${leakcheck}:halt_on_error=1 \
           UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
           "./$bin" > "/tmp/san-${name}-run.log" 2>&1; then
      echo "PASS: ${name}"
      pass=$((pass + 1))
      rm -f "$bin"
    else
      echo "FALHOU (runtime): ${name} — ver /tmp/san-${name}-run.log"
      fail=$((fail + 1))
      failed="${failed} ${name}"
      rm -f "$bin"
    fi
  else
    echo "FALHOU (build): ${name} — ver /tmp/san-${name}.log"
    fail=$((fail + 1))
    failed="${failed} ${name}(build)"
  fi
done

echo ""
echo "sanitizers ok: ${pass}, com falha: ${fail}${failed}"
[ "$fail" -eq 0 ]
