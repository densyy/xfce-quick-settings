#!/bin/sh
set -eu
#
# BUILD PLUGIN
#
cd "$(dirname "$0")/../.."
#
# COMPILE SOURCES
#
mkdir -p build
cc -Wall -Wextra -fPIC -O2 -shared -o build/libquicksettings.so \
  src/qs-plugin.c src/qs-util.c src/qs-ui.c src/qs-audio.c \
  src/qs-display.c src/qs-net.c src/qs-power.c src/qs-session.c \
  src/qs-notify.c src/qs-sysmon.c src/qs-popup.c src/qs-theme.c \
  src/qs-core.c src/qs-brightness.c src/qs-nightlight.c \
  src/qs-monitor.c src/qs-wifi.c src/qs-bluetooth.c src/qs-cache.c \
  $(pkg-config --cflags gtk+-3.0 libxfce4panel-2.0 libxfce4ui-2 libxfce4util-1.0 sqlite3) \
  $(pkg-config --libs gtk+-3.0 libxfce4panel-2.0 libxfce4ui-2 libxfce4util-1.0) -lsqlite3
