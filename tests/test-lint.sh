#!/bin/sh
# Gate de lint: cppcheck + fanalyzer + clang-tidy + grep-gates.
# Falha (exit != 0) se qualquer gate achar algo. Pula (com aviso) só
# ferramenta ausente na máquina.
set -eu
cd "$(dirname "$0")/.."

fail=0
bad() { echo "LINT-FAIL: $1"; fail=1; }
ok() { echo "LINT-OK: $1"; }

PKGS="gtk+-3.0 libxfce4panel-2.0 libxfce4ui-2 libxfce4util-1.0 sqlite3"
CFLAGS="$(pkg-config --cflags $PKGS)"
LIBS="$(pkg-config --libs $PKGS) -lsqlite3"

# 1. cppcheck (usa técnica local sem root quando disponível).
# Em tests/, syntaxError é suprimido: o cppcheck não entende as macros
# g_assert_cmp* do GLib (operador nu como argumento); erro de sintaxe real
# nos testes já é pego pela compilação com -Werror.
if [ -x /tmp/tools/usr/bin/cppcheck ] && [ -d /tmp/ccroot/usr/share ]; then
  unshare -rm bash -c 'export LD_LIBRARY_PATH=/tmp/tools/usr/lib; mount --bind /tmp/ccroot/usr/share /usr/share 2>/dev/null || exit 9; /tmp/tools/usr/bin/cppcheck --enable=warning,performance,portability --std=c11 -q src/ > /tmp/qs-lint-cppcheck-src.log 2>&1; /tmp/tools/usr/bin/cppcheck --enable=warning,performance,portability --std=c11 --suppress=syntaxError -q tests/ > /tmp/qs-lint-cppcheck-tests.log 2>&1; cat /tmp/qs-lint-cppcheck-src.log /tmp/qs-lint-cppcheck-tests.log > /tmp/qs-lint-cppcheck.log'
  n="$(grep -cE ':[0-9]+:[0-9]+: (error|warning|style|performance|portability)' /tmp/qs-lint-cppcheck.log || true)"
  if [ "$n" -eq 0 ]; then ok "cppcheck 0 findings"; else bad "cppcheck: $n findings (ver /tmp/qs-lint-cppcheck.log)"; fi
elif command -v cppcheck >/dev/null 2>&1; then
  cppcheck --enable=warning,performance,portability --std=c11 -q src/ > /tmp/qs-lint-cppcheck.log 2>&1 || true
  cppcheck --enable=warning,performance,portability --std=c11 --suppress=syntaxError -q tests/ >> /tmp/qs-lint-cppcheck.log 2>&1 || true
  n="$(grep -cE ':[0-9]+:[0-9]+: (error|warning|style|performance|portability)' /tmp/qs-lint-cppcheck.log || true)"
  if [ "$n" -eq 0 ]; then ok "cppcheck(PATH) 0 findings"; else bad "cppcheck: $n findings (ver /tmp/qs-lint-cppcheck.log)"; fi
else
  echo "LINT-SKIP: cppcheck ausente"
fi

# 2. gcc -fanalyzer (mesmas flags do build oficial)
if gcc -fanalyzer -Wall -Wextra -Wconversion -g -std=gnu11 -fPIC -O2 -shared \
    -D_FORTIFY_SOURCE=3 -o /tmp/qs-lint-fa.so src/*.c -Isrc \
    $CFLAGS $LIBS > /tmp/qs-lint-fa.log 2>&1; then
  nw="$(grep -ci 'warning' /tmp/qs-lint-fa.log || true)"
  if [ "$nw" -eq 0 ]; then ok "fanalyzer 0 warnings"; else bad "fanalyzer: $nw warnings (ver /tmp/qs-lint-fa.log)"; fi
else
  bad "fanalyzer: compilação falhou (ver /tmp/qs-lint-fa.log)"
fi

# 3. clang-tidy (bugprone + analyzer; só contam hits em arquivos próprios)
if command -v clang-tidy >/dev/null 2>&1; then
  export LD_LIBRARY_PATH="/tmp/tools/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  : > /tmp/qs-lint-tidy.log
  for f in src/*.c tests/test-*.c; do
    clang-tidy --checks='-*,bugprone-*,clang-analyzer-*' --quiet "$f" -- \
      -Isrc -Itests -std=gnu11 $CFLAGS >> /tmp/qs-lint-tidy.log 2>&1 || true
  done
  hits="$(grep -cE '^(src|tests)/' /tmp/qs-lint-tidy.log || true)"
  if [ "$hits" -eq 0 ]; then ok "clang-tidy 0 hits próprios"; else bad "clang-tidy: $hits hits (ver /tmp/qs-lint-tidy.log)"; fi
else
  echo "LINT-SKIP: clang-tidy ausente"
fi

# 4. grep-gates (higiene)
if grep -rn "$(printf '\t')" src/ tests/ --include='*.c' --include='*.h' --include='*.sh' | grep -q .; then
  bad "tabs encontrados em src/ ou tests/"
else
  ok "sem tabs"
fi
if grep -rnE '(^|[^:"])//' src/ tests/ --include='*.c' --include='*.h' | grep -q .; then
  bad "comentários // encontrados"
else
  ok "sem comentários //"
fi
if grep -rn 'g_print[[:space:]]*(' src/ tests/ --include='*.c' --include='*.h' | grep -q .; then
  bad "g_print() encontrado (usar g_warning/g_debug)"
else
  ok "sem g_print"
fi
if grep -rnE '\b(sprintf|strcpy|strcat|gets)[[:space:]]*\(' src/ tests/ --include='*.c' --include='*.h' | grep -q .; then
  bad "função insegura (sprintf/strcpy/strcat/gets) encontrada"
else
  ok "sem funções inseguras"
fi
if grep -rnE 'TODO|FIXME|XXX[^X]' src/ tests/ --include='*.c' --include='*.h' --include='*.sh' --exclude=test-lint.sh --exclude=test-build.sh | grep -v 'XXXXXX' | grep -q .; then
  bad "marcador TODO/FIXME/XXX encontrado"
else
  ok "sem TODOs"
fi
if grep -rnE ' +$' src/ tests/ --include='*.c' --include='*.h' --include='*.sh' | grep -q .; then
  bad "trailing whitespace encontrado"
else
  ok "sem trailing whitespace"
fi

if [ "$fail" -eq 0 ]; then echo "lint: OK"; else echo "lint: FALHOU"; fi
exit "$fail"
