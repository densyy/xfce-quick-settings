#!/bin/sh
set -eu
#
# CLEAN BUILD
#
cd "$(dirname "$0")/../.."
rm -f libquicksettings.so src/*.o src/*.d
