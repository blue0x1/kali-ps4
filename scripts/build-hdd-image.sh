#!/usr/bin/env bash
# Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
# Builds out/kali-ps4.img: an ext4 image of the Kali rootfs for booting from
# the PS4's internal drive. Upload it with the PS4's FTP (GoldHEN) to
#   /user/home/kali-ps4.img
# The PS4 then owns and fully allocates the file; the initramfs maps its blocks
# and never writes the PS4 filesystem itself.
#
# Usage: scripts/build-hdd-image.sh [SIZE]     (default 16G; ext4 sizes like 12G)
source "$(dirname "$0")/common.sh"

SIZE="${1:-16G}"
[ -f "$OUT/kali-ps4-rootfs.tar.xz" ] || die "Missing $OUT/kali-ps4-rootfs.tar.xz - run 'make rootfs' first"
ensure_builder

log "Building $OUT/kali-ps4.img ($SIZE ext4) from kali-ps4-rootfs.tar.xz"
rm -f "$OUT/kali-ps4.img"
docker run --rm \
    -v "$OUT:/out" \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" -e SIZE="$SIZE" \
    "$BUILDER_IMAGE" bash -euo pipefail -c '
        r=/work/root
        mkdir -p "$r"
        tar -xpJf /out/kali-ps4-rootfs.tar.xz -C "$r" --numeric-owner
        # Mounted by the initramfs, not from fstab.
        mke2fs -q -t ext4 -L KALI-PS4 -m 1 -d "$r" /out/kali-ps4.img "$SIZE"
        e2fsck -fn /out/kali-ps4.img > /dev/null
        chown "$HOST_UID:$HOST_GID" /out/kali-ps4.img
    '
ok "Image: $OUT/kali-ps4.img ($(du -h --apparent-size "$OUT/kali-ps4.img" | cut -f1), $(du -h "$OUT/kali-ps4.img" | cut -f1) used)"
