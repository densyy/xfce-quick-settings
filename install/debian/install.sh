#!/bin/sh
set -eu
#
# INSTALL PLUGIN
#
cd "$(dirname "$0")"
#
# RUN STEPS
#
sh ./deps.sh
sh ./build.sh
sh ./install-desktop.sh
echo "Done. Add 'Quick Settings' from the XFCE panel item list."
