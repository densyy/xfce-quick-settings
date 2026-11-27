#!/bin/sh
set -eu
#
# BUILD PLUGIN
#
# Compila o plugin e grava o artefato versionado em
# release/x86_64-linux/libquicksettings.so (rastreador no git).
# O install NUNCA builda: só instala o artefato já gerado.
#
cd "$(dirname "$0")/.."
#
# COMPILE SOURCES
#
mkdir -p release/x86_64-linux
cc -Wall -Wextra -Wconversion \
  -Werror=implicit-function-declaration \
  -Werror=incompatible-pointer-types \
  -Werror=int-conversion \
  -g -std=gnu11 -fPIC -O2 -shared \
  -fstack-protector-strong -fstack-clash-protection -fcf-protection \
  -D_FORTIFY_SOURCE=3 \
  -o release/x86_64-linux/libquicksettings.so \
  src/qs-plugin.c src/qs-util.c src/qs-ui.c src/qs-audio.c \
  src/qs-display.c src/qs-net.c src/qs-power.c src/qs-session.c \
  src/qs-notify.c src/qs-sysmon.c src/qs-popup.c src/qs-theme.c \
  src/qs-core.c src/qs-brightness.c src/qs-nightlight.c \
  src/qs-monitor.c src/qs-wifi.c src/qs-bluetooth.c src/qs-cache.c \
  src/qs-bt-backend.c src/qs-bt-signal.c src/qs-bt-agent.c src/qs-wifi-backend.c src/qs-notify-store.c \
  src/qs-audio-backend.c src/qs-sysmon-data.c src/qs-monitor-data.c \
  $(pkg-config --cflags gtk+-3.0 libxfce4panel-2.0 libxfce4ui-2 libxfce4util-1.0 sqlite3) \
  $(pkg-config --libs gtk+-3.0 libxfce4panel-2.0 libxfce4ui-2 libxfce4util-1.0) -lsqlite3 \
  -Wl,-z,relro,-z,now -Wl,-z,noexecstack
