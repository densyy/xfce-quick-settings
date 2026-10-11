#!/bin/sh
set -eu
#
# UNINSTALL PLUGIN
#
if [ "$(id -u)" -eq 0 ]; then
  SUDO=""
else
  SUDO="sudo"
fi
#
# REMOVE FILES
#
PLUGIN_DIR="$(pkg-config --variable=libdir libxfce4panel-2.0 2>/dev/null || echo /usr/lib)/xfce4/panel/plugins"
DESKTOP_DIR="/usr/share/xfce4/panel/plugins"
$SUDO rm -f "$PLUGIN_DIR/libquicksettings.so"
$SUDO rm -f "$DESKTOP_DIR/quicksettings.desktop"
$SUDO rm -f /etc/udev/rules.d/90-quicksettings-backlight.rules
$SUDO udevadm control --reload-rules 2>/dev/null || true
#
# RELOAD PANEL
#
if command -v xfce4-panel >/dev/null 2>&1; then
  xfce4-panel -r || true
fi
