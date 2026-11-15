#!/bin/sh
# Tries three ways of turning the screen off, 4 seconds each. Watch the screen and note which go FULLY dark.
# Run from SSH:  sh /mnt/SDCARD/Apps/Vibe/screentest.sh
cd "$(dirname "$0")" || exit 1
export LD_LIBRARY_PATH="/usr/trimui/lib:$PWD/lib:$LD_LIBRARY_PATH"
./vibe --screentest
