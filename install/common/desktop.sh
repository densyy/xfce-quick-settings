#!/bin/sh
set -eu
#
# INSTALL DESKTOP
#
if [ "$(id -u)" -eq 0 ]; then
  SUDO=""
else
  SUDO="sudo"
fi
cd "$(dirname "$0")/../.."
#
# RESOLVE DIRS
#
PLUGIN_DIR="$(pkg-config --variable=libdir libxfce4panel-2.0)/xfce4/panel/plugins"
DESKTOP_DIR="/usr/share/xfce4/panel/plugins"
TMP_DESKTOP="/tmp/quicksettings.desktop"
#
# GENERATE DESKTOP
#
cat > "$TMP_DESKTOP" <<EOF
[Xfce Panel]
Type=X-XFCE-PanelPlugin
Name=Quick Settings
Version=1.3.1
Comment=Quick access to Wi-Fi, Bluetooth, brightness, volume and power settings for the Xfce panel
Icon=preferences-system-symbolic
X-XFCE-Module=quicksettings
X-XFCE-API=2.0
X-XFCE-Internal=false
X-XFCE-Unique=true
EOF
#
# INSTALL FILES (usa o artefato já buildado em release/; nunca builda)
#
SO="release/x86_64-linux/libquicksettings.so"
if [ ! -f "$SO" ]; then
  echo "ERRO: $SO não existe — rode 'make build' antes de instalar." >&2
  exit 1
fi
$SUDO install -d "$PLUGIN_DIR"
$SUDO install -m 755 "$SO" "$PLUGIN_DIR/libquicksettings.so"
$SUDO install -d "$DESKTOP_DIR"
$SUDO install -m 644 "$TMP_DESKTOP" "$DESKTOP_DIR/quicksettings.desktop"
rm -f "$TMP_DESKTOP"
#
# BACKLIGHT RULE
#
$SUDO sh -c 'cat > /etc/udev/rules.d/90-quicksettings-backlight.rules <<EOF
ACTION=="add", SUBSYSTEM=="backlight", RUN+="/bin/chgrp video /sys/class/backlight/%k/brightness"
ACTION=="add", SUBSYSTEM=="backlight", RUN+="/bin/chmod g+w /sys/class/backlight/%k/brightness"
EOF'
$SUDO udevadm control --reload-rules 2>/dev/null || true
$SUDO udevadm trigger --subsystem-match=backlight 2>/dev/null || true
REAL_USER="${SUDO_USER:-$(id -un)}"
if [ "$REAL_USER" != "root" ]; then
  $SUDO usermod -aG video "$REAL_USER" 2>/dev/null || true
fi
#
# RELOAD PANEL
#
if command -v xfce4-panel >/dev/null 2>&1; then
  xfce4-panel -r || true
fi
