#!/bin/sh
# One command build. Needs Docker + internet. Output: dist/Vibe (with USB) and dist/Vibe-NoUSB, plus zips
set -e
cd "$(dirname "$0")"
docker build -t trimui-vibe-build .
docker run --rm --dns 8.8.8.8 --dns 1.1.1.1 -v "$PWD":/work -w /work trimui-vibe-build bash ./docker-build.sh
echo "Done. Copy ONE of dist/Vibe or dist/Vibe-NoUSB to /mnt/SDCARD/Apps/ (or unzip its zip there)."
