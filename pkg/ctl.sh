#!/bin/sh
# Control a backgrounded player: ./ctl.sh toggle|play|pause|next|prev|show|quit
echo "$1" > /tmp/vibe.ctl
