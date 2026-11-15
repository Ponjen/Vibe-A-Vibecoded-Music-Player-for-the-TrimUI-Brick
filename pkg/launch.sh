#!/bin/sh
# Works on Stock OS and CrossMix (both launch /mnt/SDCARD/Apps/<name>/launch.sh).
APPDIR="$(dirname "$0")"
cd "$APPDIR" || exit 1
export LD_LIBRARY_PATH="/usr/trimui/lib:$APPDIR/lib:$LD_LIBRARY_PATH"
export SDL_NOMOUSE=1

CTL=/tmp/vibe.ctl; PIDF=/tmp/vibe.pid; UIF=/tmp/vibe.ui

if [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null; then
    # Already playing in the background: bring its UI back.
    touch "$UIF"
    echo show > "$CTL"
else
    rm -f "$PIDF" "$CTL"
    touch "$UIF"
    # setsid so the player survives this script exiting (background play)
    ( setsid ./vibe >> app.log 2>&1 < /dev/null & )
fi

# Stay alive while the player owns the screen so the OS keeps the launcher hidden.
i=0
while [ -f "$UIF" ]; do
    if [ -f "$PIDF" ]; then
        kill -0 "$(cat "$PIDF")" 2>/dev/null || break
    else
        i=$((i + 1)); [ "$i" -gt 100 ] && break
    fi
    sleep 0.3 2>/dev/null || sleep 1
done
rm -f "$UIF"
exit 0
