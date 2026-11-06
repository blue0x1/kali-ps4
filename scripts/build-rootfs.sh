#!/usr/bin/env bash
# Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
# Builds out/kali-ps4-rootfs.tar.xz: Kali userland + PS4 kernel modules + firmware.
# Needs build-kernel.sh to have run first.
source "$(dirname "$0")/common.sh"

[ -d "$BUILD/modules/lib/modules" ] || die "No kernel modules yet - run 'make kernel' first"

log "Building Kali rootfs image (PROFILE=$PROFILE TOOLS=$TOOLS)"
docker build -t "$ROOTFS_IMAGE" --build-arg PROFILE="$PROFILE" --build-arg TOOLS="$TOOLS" -f "$ROOT/docker/rootfs.Dockerfile" "$ROOT"

log "Exporting container filesystem"
cid="$(docker create "$ROOTFS_IMAGE")"
trap 'docker rm -f "$cid" >/dev/null' EXIT
docker export "$cid" -o "$BUILD/rootfs.tar"

ensure_builder
log "Adding kernel modules and firmware, compressing"
FIRMWARE="$BUILD/ps4-linux/extra_firmware"
docker run --rm \
    -v "$BUILD:/build:ro" \
    -v "$ROOT/overlay:/overlay:ro" \
    -v "$OUT:/out" \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    -e HAVE_FW="$([ -d "$FIRMWARE" ] && echo 1 || echo 0)" \
    "$BUILDER_IMAGE" bash -euo pipefail -c '
        r=/work/root
        mkdir -p "$r"
        tar -xpf /build/rootfs.tar -C "$r" --numeric-owner

        cp -a /build/modules/lib/modules/. "$r/usr/lib/modules/"
        if [ "$HAVE_FW" = 1 ]; then
            cp -a /build/ps4-linux/extra_firmware/. "$r/usr/lib/firmware/"
        fi
        # The initramfs covers these with the console GPU firmware and EDID
        # that ps4-linux-loader extracts at boot, so they only need to exist.
        mkdir -p "$r/usr/lib/firmware/amdgpu" "$r/usr/lib/firmware/edid"

        # docker export leaves its own runtime versions of these files.
        cp /overlay/etc/hostname /overlay/etc/hosts "$r/etc/"
        rm -f "$r/etc/resolv.conf" && : > "$r/etc/resolv.conf"
        rm -f "$r/.dockerenv"

        tar -cpf - -C "$r" --numeric-owner . | xz -T0 -6 > /out/kali-ps4-rootfs.tar.xz
        chown "$HOST_UID:$HOST_GID" /out/kali-ps4-rootfs.tar.xz
    '

rm -f "$BUILD/rootfs.tar"
ok "Rootfs: $OUT/kali-ps4-rootfs.tar.xz ($(du -h "$OUT/kali-ps4-rootfs.tar.xz" | cut -f1))"
