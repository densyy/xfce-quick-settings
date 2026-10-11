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
sh ../common/build.sh
sh ../common/install-desktop.sh
echo "Done. Add 'Quick Settings' from the XFCE panel item list."
