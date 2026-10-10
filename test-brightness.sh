#!/usr/bin/env sh
# Teste completo do brilho (~10s). MUDA a iluminacao de proposito,
# com piso seguro, e restaura ao final (inclusive com Ctrl+C).
# Uso: sh test-brightness.sh
# Log: /tmp/qs-brightness-test.log
# Pisos: HW nunca abaixo de 60%, gamma xrandr nunca abaixo de 0.8.
set -u

LOG=/tmp/qs-brightness-test.log
: > "$LOG"
log() { echo "$@" | tee -a "$LOG"; }

STEP_S=2
TEST_HI=80
TEST_LO=60
TEST_GAMMA=0.8

ORIG_BRI=""
ORIG_SYS_DEV=""
ORIG_SYS_RAW=""
ORIG_GAMMA=""
OUT=""

get_bri() {
  if command -v brightnessctl >/dev/null 2>&1; then
    brightnessctl -m 2>/dev/null | cut -d, -f4 | tr -dc '0-9' | head -n 1
  else
    for d in /sys/class/backlight/*; do
      [ -e "$d/max_brightness" ] || continue
      mx=$(cat "$d/max_brightness" 2>/dev/null) || continue
      cu=$(cat "$d/brightness" 2>/dev/null) || continue
      if [ -n "$mx" ] && [ "$mx" -gt 0 ] 2>/dev/null; then
        echo $((cu * 100 / mx))
        return
      fi
    done
  fi
}

get_gamma() {
  command -v xrandr >/dev/null 2>&1 || return
  xrandr --verbose 2>/dev/null | grep "Brightness:" | head -n 1 | awk '{print $2}'
}

get_output() {
  command -v xrandr >/dev/null 2>&1 || return
  xrandr 2>/dev/null | grep " connected" | cut -d" " -f1 | head -n 1
}

save_originals() {
  ORIG_BRI=$(get_bri || true)
  ORIG_GAMMA=$(get_gamma || true)
  OUT=$(get_output || true)
  for d in /sys/class/backlight/*; do
    [ -e "$d/brightness" ] || continue
    ORIG_SYS_DEV="$d"
    ORIG_SYS_RAW=$(cat "$d/brightness" 2>/dev/null || true)
    break
  done
  [ -z "$ORIG_BRI" ] && ORIG_BRI=$TEST_HI
  [ -z "$ORIG_GAMMA" ] && ORIG_GAMMA=1.0
}

restore() {
  log "--- restore ---"
  if command -v brightnessctl >/dev/null 2>&1 && [ -n "$ORIG_BRI" ]; then
    log "restore brightnessctl -> $ORIG_BRI%"
    brightnessctl set "$ORIG_BRI%" >>"$LOG" 2>&1 || log "WARN restore brightnessctl"
  fi
  if [ -n "$ORIG_SYS_DEV" ] && [ -n "$ORIG_SYS_RAW" ]; then
    if [ -w "$ORIG_SYS_DEV/brightness" ]; then
      echo "$ORIG_SYS_RAW" > "$ORIG_SYS_DEV/brightness" 2>/dev/null \
        && log "restore sysfs $ORIG_SYS_DEV -> $ORIG_SYS_RAW" \
        || log "WARN restore sysfs direto"
    else
      sudo sh -c "echo '$ORIG_SYS_RAW' > '$ORIG_SYS_DEV/brightness'" >>"$LOG" 2>&1 \
        && log "restore sysfs via sudo -> $ORIG_SYS_RAW" \
        || log "WARN restore sysfs sudo (rode: sudo sh -c 'echo X > ...')"
    fi
  fi
  if [ -n "$OUT" ] && [ -n "${DISPLAY:-}" ]; then
    log "restore gamma $OUT -> $ORIG_GAMMA"
    xrandr --output "$OUT" --brightness "$ORIG_GAMMA" >>"$LOG" 2>&1 \
      || log "WARN restore xrandr"
  fi
  log "fim restore"
}
trap restore EXIT INT TERM

log "== qs brightness FULL test =="
log "data: $(date '+%F %T')"
log "brightnessctl: $(command -v brightnessctl 2>/dev/null || echo ausente)"
brightnessctl -m 2>&1 | head -n 3 | tee -a "$LOG" || true
log "backlight:"
ls -l /sys/class/backlight/ 2>&1 | tee -a "$LOG" || true
log "xrandr/xbacklight: $(command -v xrandr 2>/dev/null || echo sem-xrandr) / $(command -v xbacklight 2>/dev/null || echo sem-xbacklight)"
log "grupos: $(id 2>/dev/null | tr ',' '\n' | grep -E 'video|sudo' || echo sem-video-sem-sudo)"

save_originals
log "ORIG brilho=${ORIG_BRI}% sysfs=${ORIG_SYS_DEV:-n/a}=${ORIG_SYS_RAW:-n/a} gamma=${ORIG_GAMMA} out=${OUT:-n/a}"

log "--- 1/5 HW via brightnessctl -> ${TEST_HI}% (${STEP_S}s, OLHE A TELA) ---"
if command -v brightnessctl >/dev/null 2>&1; then
  brightnessctl set ${TEST_HI}% 2>&1 | tee -a "$LOG" || log "FAIL set 80"
  log "mudou? (esperado: tela em 80%)"
else
  log "SKIP sem brightnessctl"
fi
sleep $STEP_S

log "--- 2/5 HW via brightnessctl -> ${TEST_LO}% (${STEP_S}s) ---"
if command -v brightnessctl >/dev/null 2>&1; then
  brightnessctl set ${TEST_LO}% 2>&1 | tee -a "$LOG" || log "FAIL set 60"
else
  log "SKIP"
fi
sleep $STEP_S

log "--- 3/5 HW via sysfs/xbacklight (${STEP_S}s) ---"
if [ -n "$ORIG_SYS_DEV" ]; then
  mx=$(cat "$ORIG_SYS_DEV/max_brightness" 2>/dev/null || echo "")
  if [ -n "$mx" ] && [ "$mx" -gt 0 ] 2>/dev/null; then
    target=$((mx * TEST_LO / 100))
    log "sysfs $ORIG_SYS_DEV -> $target (60% de $mx)"
    if [ -w "$ORIG_SYS_DEV/brightness" ]; then
      echo "$target" > "$ORIG_SYS_DEV/brightness" 2>/dev/null \
        && log "OK escrita direta" || log "FAIL escrita direta"
    else
      sudo sh -c "echo '$target' > '$ORIG_SYS_DEV/brightness'" >>"$LOG" 2>&1 \
        && log "OK via sudo" || log "FAIL/SEM sudo (sem mudanca aqui)"
    fi
  fi
elif command -v xbacklight >/dev/null 2>&1; then
  log "xbacklight -> ${TEST_LO}"
  xbacklight -set $TEST_LO 2>&1 | tee -a "$LOG" || log "FAIL xbacklight"
else
  log "SKIP sem sysfs nem xbacklight"
fi
sleep $STEP_S

log "--- 4/5 gamma xrandr $OUT -> $TEST_GAMMA (${STEP_S}s) ---"
if [ -n "$OUT" ] && [ -n "${DISPLAY:-}" ]; then
  log "AVISO: gamma escurece tudo; Fn nao desfaz, mas o restore refaz"
  xrandr --output "$OUT" --brightness $TEST_GAMMA 2>&1 | tee -a "$LOG" || log "FAIL gamma"
else
  log "SKIP sem DISPLAY/output (rode no X)"
fi
sleep $STEP_S

log "--- 5/5 restore + confere (${STEP_S}s) ---"
restore
trap - EXIT INT TERM
sleep $STEP_S

NOW_BRI=$(get_bri || true)
NOW_GAMMA=$(get_gamma || true)
log "AGORA brilho=${NOW_BRI:-(?)} gamma=${NOW_GAMMA:-(?)} (orig ${ORIG_BRI}% / $ORIG_GAMMA)"
OK=1
if [ -n "$ORIG_BRI" ] && [ -n "$NOW_BRI" ]; then
  D=$((ORIG_BRI > NOW_BRI ? ORIG_BRI - NOW_BRI : NOW_BRI - ORIG_BRI))
  if [ "$D" -gt 10 ]; then log "FAIL: brilho nao voltou (orig=$ORIG_BRI agora=$NOW_BRI)"; OK=0
  else log "OK: brilho restaurado"; fi
else
  log "WARN: sem leitura p/ conferir brilho"
fi
if [ "$OK" -eq 1 ]; then log "RESULTADO: PASS"; else log "RESULTADO: FAIL"; fi
log "log em $LOG"
