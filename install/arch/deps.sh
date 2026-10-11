#!/bin/sh
set -eu
#
# INSTALL DEPS
#
if [ "$(id -u)" -eq 0 ]; then
  SUDO=""
else
  SUDO="sudo"
fi
#
# SYSTEM PACKAGES
#
$SUDO pacman -Sy --needed --noconfirm \
  base-devel \
  pkgconf \
  gtk3 \
  xfce4-panel \
  libxfce4ui \
  libxfce4util \
  sqlite \
  brightnessctl \
  upower \
  networkmanager \
  bluez \
  libpulse \
  xorg-xrandr
