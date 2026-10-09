#!/usr/bin/env sh
set -eu

if [ "$(id -u)" -eq 0 ]; then
  SUDO=""
else
  SUDO="sudo"
fi

echo "Uninstalling Quick Settings..."
$SUDO make uninstall

if command -v xfce4-panel >/dev/null 2>&1; then
  echo "Reloading XFCE panel..."
  xfce4-panel -r || true
fi

echo "Done."
