#!/bin/sh
# Matriz de build: -O0/-O2/-Os × cc/gcc/clang com -Werror (estrito, estilo dev),
# + checksec + nm + reprodutibilidade. Falha (exit != 0) em qualquer problema.
set -eu
cd "$(dirname "$0")/.."

fail=0
bad() { echo "BUILD-FAIL: $1"; fail=1; }
ok() { echo "BUILD-OK: $1"; }

# clang do sistema precisa das llvm-libs 22 extraídas em /tmp/tools (sem root)
if [ -d /tmp/tools/usr/lib ]; then
  export LD_LIBRARY_PATH="/tmp/tools/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

PKGS="gtk+-3.0 libxfce4panel-2.0 libxfce4ui-2 libxfce4util-1.0 sqlite3"
CFLAGS="$(pkg-config --cflags $PKGS)"
LIBS="$(pkg-config --libs $PKGS) -lsqlite3"
BASE="-Wall -Wextra -Wconversion -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -Werror=int-conversion -Werror -g -std=gnu11 -fPIC -shared -fstack-protector-strong -fstack-clash-protection -fcf-protection -Wl,-z,relro,-z,now -Wl,-z,noexecstack"
mkdir -p /tmp/qs-matrix

# 1. matriz de compilação (fortify exige -O1+, por isso fora do -O0)
for cc in cc gcc clang; do
  for opt in -O0 -O2 -Os; do
    tag="${cc}_${opt#-}"
    if [ "$opt" = "-O0" ]; then F="$BASE"; else F="$BASE -D_FORTIFY_SOURCE=3"; fi
    if $cc $F $opt -o "/tmp/qs-matrix/libqs-$tag.so" src/*.c -Isrc \
        $CFLAGS $LIBS > "/tmp/qs-matrix/$tag.log" 2>&1; then
      ok "compila $cc $opt (0 warnings)"
    else
      bad "compila $cc $opt (ver /tmp/qs-matrix/$tag.log)"
      tail -5 "/tmp/qs-matrix/$tag.log"
    fi
  done
done

REF=/tmp/qs-matrix/libqs-gcc_O2.so
if [ -f "$REF" ]; then
  # 2. hardening (checksec local sem root quando disponível)
  if [ -x /tmp/tools/usr/bin/checksec ]; then
    out="$(/tmp/tools/usr/bin/checksec --file="$REF" 2>&1 || true)"
    echo "$out" | grep -q "Full RELRO" && ok "RELRO full" || bad "sem Full RELRO"
    echo "$out" | grep -q "Canary found" && ok "stack canary" || bad "sem stack canary"
    echo "$out" | grep -q "NX enabled" && ok "NX enabled" || bad "sem NX"
  else
    echo "BUILD-SKIP: checksec ausente"
  fi

  # 3. símbolos: entry point exportado + nenhum símbolo interno undefined
  if nm -D --defined-only "$REF" | grep -q "xfce_panel_module_construct"; then
    ok "símbolo xfce_panel_module_construct exportado"
  else
    bad "símbolo xfce_panel_module_construct ausente"
  fi
  und="$(nm -D -u "$REF" | grep -E ' U (qs_|bright_|power_|sysmon_|wifi_|bt_|audio_|pactl_|notify_|flat_|notice_|unescape_|valid_|header_|snap_|battery_|volume_|cache_|display_|night_|mon_)' || true)"
  if [ -z "$und" ]; then
    ok "nenhum símbolo interno undefined"
  else
    bad "símbolos internos undefined: $und"
  fi

  # 4. reprodutibilidade: dois builds idênticos → mesmo sha256
  if gcc $BASE -D_FORTIFY_SOURCE=3 -O2 -o /tmp/qs-matrix/libqs-repro.so src/*.c \
      -Isrc $CFLAGS $LIBS > /tmp/qs-matrix/repro.log 2>&1; then
    a="$(sha256sum "$REF" | cut -d' ' -f1)"
    b="$(sha256sum /tmp/qs-matrix/libqs-repro.so | cut -d' ' -f1)"
    if [ "$a" = "$b" ]; then ok "build reprodutível ($a)"; else bad "builds diferem: $a vs $b"; fi
  else
    bad "build de reprodutibilidade falhou"
  fi
else
  bad "artefato de referência ausente (matriz gcc -O2 falhou)"
fi

if [ "$fail" -eq 0 ]; then echo "build-matrix: OK"; else echo "build-matrix: FALHOU"; fi
exit "$fail"
