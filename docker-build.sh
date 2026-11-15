#!/bin/bash
# Runs INSIDE the container.
set -euo pipefail
cd /work

CROSS_COMPILE="${CROSS_COMPILE:-$(ls /opt/*/bin/*-gcc 2>/dev/null | head -n1 | sed 's/gcc$//')}"
[ -n "$CROSS_COMPILE" ] || { echo "No aarch64 cross compiler found in /opt"; exit 1; }
CC="${CROSS_COMPILE}gcc"
SYSROOT="$($CC -print-sysroot)"
echo "Using $CC (sysroot $SYSROOT)"

DEPS=/work/build/deps; PREFIX=$DEPS/install
mkdir -p "$PREFIX"; cd "$DEPS"

# download helper: tries each URL in turn; a pre-placed file in build/deps is used as-is
fetch() {  # fetch <outfile> <url>...
  local out="$1"; shift
  [ -s "$out" ] && return 0
  for u in "$@"; do
    echo "Downloading $u"
    if curl -fL --retry 3 --connect-timeout 20 -o "$out.part" "$u" && [ -s "$out.part" ]; then mv "$out.part" "$out"; return 0; fi
  done
  echo "!! Could not download $out. Download it manually into $DEPS and re-run ./build.sh"; return 1
}
unpack() {  # unpack <tarball> <dir>
  rm -rf "$2"; mkdir -p "$2"; tar xf "$1" -C "$2" --strip-components=1
}

# ---- zlib (needed for PNG album art) ----
if [ ! -f "$PREFIX/lib/libz.a" ]; then
  fetch zlib-1.3.1.tar.gz https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz https://zlib.net/zlib-1.3.1.tar.gz
  unpack zlib-1.3.1.tar.gz zlib-1.3.1
  (cd zlib-1.3.1 && CC="$CC" AR="${CROSS_COMPILE}ar" RANLIB="${CROSS_COMPILE}ranlib" \
     CFLAGS="-O2 -fPIC" ./configure --prefix="$PREFIX" --static && make -j"$(nproc)" && make install)
fi

# ---- FFmpeg: minimal, static, decoders for the six formats + cover art ----
FFSTAMP="muxers-2"      # bump when the FFmpeg configure line changes so an old build gets redone
if [ ! -f "$PREFIX/lib/libavcodec.a" ] || [ "$(cat "$PREFIX/.ffstamp" 2>/dev/null)" != "$FFSTAMP" ]; then
  fetch ffmpeg-6.1.1.tar.gz https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n6.1.1.tar.gz \
        https://ffmpeg.org/releases/ffmpeg-6.1.1.tar.gz
  unpack ffmpeg-6.1.1.tar.gz ffmpeg-6.1.1
  (cd ffmpeg-6.1.1 && (make distclean >/dev/null 2>&1 || true) && ./configure --prefix="$PREFIX" \
     --enable-cross-compile --arch=aarch64 --target-os=linux --cross-prefix="$CROSS_COMPILE" \
     --disable-everything --disable-autodetect --disable-programs --disable-doc --disable-debug \
     --disable-avdevice --disable-avfilter --disable-network \
     --enable-static --disable-shared --enable-pic --enable-zlib \
     --enable-protocol=file \
     --enable-demuxer=mp3,flac,ogg,mov,wav,image_jpeg_pipe,image_png_pipe,image_webp_pipe --enable-muxer=mp3,flac,ogg,opus,ipod,wav \
     --enable-parser=mpegaudio,flac,vorbis,opus,aac,mjpeg,png,bmp \
     --enable-decoder=mp3float,mp3,flac,vorbis,opus,aac,alac,pcm_s16le,pcm_s24le,pcm_s32le,pcm_u8,pcm_f32le,pcm_f64le,pcm_s16be,pcm_s24be,mjpeg,png,bmp,webp,vp8 \
     --extra-cflags="-O2 -I$PREFIX/include" --extra-ldflags="-L$PREFIX/lib" \
   && make -j"$(nproc)" && make install) && echo "$FFSTAMP" > "$PREFIX/.ffstamp"
fi

# ---- app: two builds, with and without the USB feature ----
cd /work
make clean
make CROSS_COMPILE="$CROSS_COMPILE" SYSROOT="$SYSROOT" DEPS="$PREFIX"
cp vibe /tmp/vibe_usb
make clean
make CROSS_COMPILE="$CROSS_COMPILE" SYSROOT="$SYSROOT" DEPS="$PREFIX" EXTRA_CFLAGS=-DNO_USB
cp vibe /tmp/vibe_nousb

# ---- package ----
OUT=dist/Vibe
rm -rf dist; mkdir -p "$OUT/fonts"
cp /tmp/vibe_usb "$OUT/vibe"
cp pkg/launch.sh pkg/config.json pkg/icon.png pkg/ctl.sh pkg/buttons.ini pkg/usbmusic.sh pkg/themes.ini pkg/screentest.sh "$OUT/"
chmod +x "$OUT/launch.sh" "$OUT/ctl.sh" "$OUT/usbmusic.sh" "$OUT/screentest.sh" "$OUT/vibe"
echo "Fetching fonts: Noto Sans CJK (Japanese + Korean + Chinese + Latin/Cyrillic/Greek) and fallbacks..."
getfont() { curl -fL --retry 3 --connect-timeout 20 -o "$1" "$2" || { echo "!! failed: $2"; rm -f "$1"; }; }
getfont "$OUT/fonts/font.otf" https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/Japanese/NotoSansCJKjp-Regular.otf
N=https://github.com/notofonts/notofonts.github.io/raw/main/fonts
getfont "$OUT/fonts/fb_1_arabic.ttf"     $N/NotoSansArabic/hinted/ttf/NotoSansArabic-Regular.ttf
getfont "$OUT/fonts/fb_2_hebrew.ttf"     $N/NotoSansHebrew/hinted/ttf/NotoSansHebrew-Regular.ttf
getfont "$OUT/fonts/fb_3_thai.ttf"       $N/NotoSansThai/hinted/ttf/NotoSansThai-Regular.ttf
getfont "$OUT/fonts/fb_4_devanagari.ttf" $N/NotoSansDevanagari/hinted/ttf/NotoSansDevanagari-Regular.ttf
getfont "$OUT/fonts/fb_5_symbols2.ttf"   $N/NotoSansSymbols2/hinted/ttf/NotoSansSymbols2-Regular.ttf
getfont "$OUT/fonts/fb_6_symbols.ttf"    $N/NotoSansSymbols/hinted/ttf/NotoSansSymbols-Regular.ttf
getfont "$OUT/fonts/fb_9_dejavu.ttf"     https://github.com/dejavu-fonts/dejavu-fonts/raw/master/ttf/DejaVuSans.ttf
echo "Fetching the downloader: yt-dlp, QuickJS and a static ffmpeg with MP3 (LAME) support..."
mkdir -p "$OUT/bin"
getfile() { local out="$1"; shift; for u in "$@"; do curl -fL --retry 3 --connect-timeout 20 -o "$out" "$u" && [ -s "$out" ] && return 0; done; rm -f "$out"; echo "!! failed: $*"; return 1; }
FF=$(mktemp -d)
# yt-dlp: the latest standalone aarch64 build, checked against the release's published SHA-256
getfile "$OUT/bin/yt-dlp" https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux_aarch64
if getfile "$FF/SUMS" https://github.com/yt-dlp/yt-dlp/releases/latest/download/SHA2-256SUMS; then
  want=$(awk '$2=="yt-dlp_linux_aarch64"{print $1}' "$FF/SUMS")
  have=$(sha256sum "$OUT/bin/yt-dlp" 2>/dev/null | cut -d' ' -f1)
  if [ -n "$want" ] && [ "$want" != "$have" ]; then echo "!! yt-dlp checksum mismatch - discarding it"; rm -f "$OUT/bin/yt-dlp"; fi
fi
# QuickJS-NG (yt-dlp's JavaScript runtime), pinned to the version BoomBrick ships, static build, checked
QJS_VER=v0.17.0
QJS_SHA=3372133484edf50a69f3c67903af41206d22a061e930e3cfb63269272ef56d2e
getfile "$OUT/bin/qjs" https://github.com/quickjs-ng/quickjs/releases/download/$QJS_VER/qjs-linux-aarch64
if [ -s "$OUT/bin/qjs" ] && ! echo "$QJS_SHA  $OUT/bin/qjs" | sha256sum -c --quiet -; then echo "!! qjs checksum mismatch - discarding it"; rm -f "$OUT/bin/qjs"; fi
if getfile "$FF/ff.tar.xz" https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-arm64-static.tar.xz && tar xf "$FF/ff.tar.xz" -C "$FF"; then
  cp "$FF"/ffmpeg-*-static/ffmpeg "$FF"/ffmpeg-*-static/ffprobe "$OUT/bin/"
else
  echo "johnvansickle.com unavailable, trying a fallback ffmpeg build (no ffprobe)..."
  getfile "$OUT/bin/ffmpeg" https://github.com/eugeneware/ffmpeg-static/releases/download/b6.1.1/ffmpeg-linux-arm64
fi
getfile "$OUT/bin/cacert.pem" https://curl.se/ca/cacert.pem || echo "!! no CA bundle: search-result covers may not load"
chmod +x "$OUT"/bin/* 2>/dev/null || true
for f in yt-dlp qjs ffmpeg; do [ -s "$OUT/bin/$f" ] || echo "!! bin/$f is missing - the downloader will not work until you put it there (see README)"; done
[ -s "$OUT/fonts/font.otf" ] || echo "!! Main font missing: put a CJK .otf/.ttf at Vibe/fonts/font.otf"
# the no-USB variant: same app, USB menu entry compiled out, no usbmusic.sh
cp -a dist/Vibe dist/Vibe-NoUSB
cp /tmp/vibe_nousb dist/Vibe-NoUSB/vibe
rm -f dist/Vibe-NoUSB/usbmusic.sh
(cd dist && zip -qr Vibe.zip Vibe && zip -qr Vibe-NoUSB.zip Vibe-NoUSB)
echo "Built dist/Vibe (with USB) and dist/Vibe-NoUSB"
