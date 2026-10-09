#!/usr/bin/env sh
set -eu

if [ "$(id -u)" -eq 0 ]; then
  SUDO=""
else
  SUDO="sudo"
fi

echo "Installing build dependencies..."
if command -v apt-get >/dev/null 2>&1; then
  $SUDO apt-get update
  # Repair broken third-party deps first: apt refuses new installs while
  # held/broken packages exist (e.g. rustdesk blocking the chain above).
  $SUDO apt-get install -y -f || true
  if ! $SUDO apt-get install -y \
    build-essential \
    pkg-config \
    libgtk-3-dev \
    libxfce4panel-2.0-dev \
    libxfce4ui-2-dev \
    libxfce4util-dev; then
    echo "Dependency install failed."
    echo "Repair manually with: sudo apt --fix-broken install"
    echo "If a third-party package blocks it (see the error above), remove it first, e.g.: sudo apt remove rustdesk"
    exit 1
  fi
elif command -v pacman >/dev/null 2>&1; then
  $SUDO pacman -Sy --needed --noconfirm \
    base-devel \
    pkgconf \
    gtk3 \
    xfce4-panel
else
  echo "No supported package manager found. Install GTK3 + xfce4-panel dev files manually, then run make && sudo make install."
fi

echo "Building Quick Settings..."
make

echo "Installing panel plugin..."
$SUDO make install

if command -v xfce4-panel >/dev/null 2>&1; then
  echo "Reloading XFCE panel..."
  xfce4-panel -r || true
fi

echo "Done. Add 'Quick Settings' from the XFCE panel item list."
