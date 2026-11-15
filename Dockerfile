# Cross-compile environment for the TrimUI Brick (tg5040, aarch64).
# If this image name ever changes, try: BASE=shauninman/union-tg5040-toolchain
ARG BASE=ghcr.io/loveretro/tg5040-toolchain:latest
FROM ${BASE}
RUN (apt-get update && apt-get install -y --no-install-recommends \
      curl ca-certificates xz-utils make zip pkg-config && rm -rf /var/lib/apt/lists/*) || true
WORKDIR /work
