#!/usr/bin/env bash
# Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
# Builds out/initramfs.cpio.gz from initramfs/init and a static busybox.
source "$(dirname "$0")/common.sh"

ensure_builder

# No GPU firmware here on purpose: ps4-linux-loader extracts the console's own
# amdgpu firmware (liverpool_* or gladius_*) into a cpio placed *before* this
# initramfs. Anything we shipped under lib/firmware/amdgpu would overwrite it.

# e2fsck (repairs the root filesystem after hard power-offs), mke2fs
# (first-boot install) and cryptsetup + dmsetup (boot from the PS4 internal
# drive), with their libraries, all from the Kali rootfs image so the
# libraries match. Falls back to the builder image
# (e2fsck only) if the rootfs image hasn't been built yet.
EXTRA="$BUILD/initramfs-extra"
rm -rf "$EXTRA"
mkdir -p "$EXTRA"
if docker image inspect "$ROOTFS_IMAGE" >/dev/null 2>&1; then
    SRC_IMAGE="$ROOTFS_IMAGE"; BINS="e2fsck mke2fs cryptsetup dmsetup"
else
    SRC_IMAGE="$BUILDER_IMAGE"; BINS="e2fsck"
    log "No $ROOTFS_IMAGE image yet: initramfs gets e2fsck only (no internal-drive install)"
fi
docker run --rm -v "$EXTRA:/x" -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" -e BINS="$BINS" \
    "$SRC_IMAGE" sh -euc '
        mkdir -p /x/bin
        for b in $BINS; do
            f="$(command -v "$b")" || { echo "missing $b" >&2; exit 1; }
            cp "$f" "/x/bin/$b"
            for l in $(ldd "$f" | grep -o "/[^ ]*"); do
                mkdir -p "/x$(dirname "$l")"
                cp -L "$l" "/x$l"
            done
        done
        chown -R "$HOST_UID:$HOST_GID" /x
    '

# Runs as root in the container so /dev/console can be created with mknod.
docker run --rm \
    -v "$ROOT/initramfs:/src:ro" \
    -v "$ROOT/assets:/assets:ro" \
    -v "$EXTRA:/extra:ro" \
    -v "$OUT:/out" \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    "$BUILDER_IMAGE" bash -euo pipefail -c '
        stage=$(mktemp -d)
        chmod 755 "$stage"
        mkdir -p "$stage"/{bin,dev,proc,sys,newroot}
        cp -a /extra/. "$stage/"
        cp /bin/busybox "$stage/bin/busybox"
        # Static helper that maps kali-ps4.img on the PS4 drive (see blockmap.c).
        gcc -O2 -Wall -static -o "$stage/bin/blockmap" /src/blockmap.c
        # Boot screen: Kali logo (assets/kali-logo-splash.rgb) + credits.
        gcc -O2 -Wall -static -o "$stage/bin/splash" /src/splash.c
        # Keeps the Slim/Pro HDMI bridge output on during the initramfs.
        gcc -O2 -Wall -static -o "$stage/bin/ps4hdmi" /src/ps4hdmi.c
        # First-boot screens: Install button and progress bar.
        gcc -O2 -Wall -Wno-format-truncation -static -pthread -o "$stage/bin/fbui" /src/fbui.c -lm
        mkdir -p "$stage/etc/kali-ps4"
        cp /assets/kali-logo-splash.rgb "$stage/etc/kali-ps4/splash.rgb"
        cp /assets/kali-logo.rgba "$stage/etc/kali-ps4/logo.rgba"
        install -m 755 /src/init "$stage/init"
        mknod -m 600 "$stage/dev/console" c 5 1
        mknod -m 666 "$stage/dev/null" c 1 3
        (cd "$stage" && find . | cpio --quiet -o -H newc -R 0:0) | gzip -9 > /out/initramfs.cpio.gz
        chown "$HOST_UID:$HOST_GID" /out/initramfs.cpio.gz
    '

ok "Initramfs: $OUT/initramfs.cpio.gz"
