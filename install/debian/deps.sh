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
$SUDO apt-get update
$SUDO apt-get install -y -f || true
$SUDO apt-get install -y \
  build-essential \
  pkg-config \
  libgtk-3-dev \
  libxfce4panel-2.0-dev \
  libxfce4ui-2-dev \
  libxfce4util-dev \
  libsqlite3-dev \
  brightnessctl \
  upower \
  network-manager \
  bluez \
  pulseaudio-utils \
  x11-xserver-utils
