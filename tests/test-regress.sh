#!/bin/sh
# Checagens estruturais de regressão (M01/M02/M08/M11, desktop keys, flags).
set -eu
cd "$(dirname "$0")/.."

fail=0
bad() { echo "REGRESS: $1"; fail=1; }

# M01: wifi_show_page não pode chamar wifi_enabled_sync (sync no main loop).
if awk '/^wifi_show_page/,/^}/' src/qs-wifi.c | grep -q "wifi_enabled_sync ("; then
  bad "M01: wifi_enabled_sync voltou a wifi_show_page"
fi

# M02: set_brightness_percent sem run_command (sync no main loop).
if awk '/^set_brightness_percent/,/^}/' src/qs-brightness.c | grep -q "run_command ("; then
  bad "M02: run_command voltou a set_brightness_percent"
fi

# M08: zero g_thread_new (usar g_thread_try_new).
if grep -rn "g_thread_new *(" src/ --include='*.c' | grep -q .; then
  bad "M08: g_thread_new encontrado em src/"
fi

# Sync permitido só em qs-util.c (run_command) e qs-wifi.c (worker thread).
if grep -rln "g_spawn_command_line_sync" src/ --include='*.c' \
  | grep -qv -e "qs-util.c" -e "qs-wifi.c"; then
  bad "g_spawn_command_line_sync fora de qs-util.c/qs-wifi.c"
fi

# M11: zero atoi (usar qs_parse_int_checked).
if grep -rn "atoi *(" src/ --include='*.c' --include='*.h' | grep -q .; then
  bad "M11: atoi encontrado em src/"
fi

# Sem shell explícito.
if grep -rn "system *(\|popen *(" src/ --include='*.c' | grep -q .; then
  bad "system()/popen() em src/"
fi

# M05: chaves do wrapper no .desktop gerado.
grep -q "X-XFCE-Internal=false" install/common/desktop.sh \
  || bad "desktop sem X-XFCE-Internal=false"
grep -q "X-XFCE-Unique=true" install/common/desktop.sh \
  || bad "desktop sem X-XFCE-Unique=true"

# M04: -Wconversion no build oficial.
grep -q "\-Wconversion" install/common/build.sh \
  || bad "build sem -Wconversion"

if [ "$fail" -eq 0 ]; then
  echo "regress: OK"
else
  echo "regress: FALHOU"
fi
exit "$fail"
