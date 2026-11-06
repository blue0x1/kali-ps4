# Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
# Kali-PS4 root filesystem. Built with `docker build`, then exported as a tarball.
FROM docker.io/kalilinux/kali-rolling

# minimal: text console only.  desktop: XFCE.
ARG PROFILE=desktop
# Kali metapackage with security tools, or "none".
ARG TOOLS=none
ENV DEBIAN_FRONTEND=noninteractive

COPY config/packages-*.txt /tmp/
RUN set -e; \
    case "$PROFILE" in \
        minimal) lists=/tmp/packages-base.txt ;; \
        desktop) lists="/tmp/packages-base.txt /tmp/packages-desktop.txt" ;; \
        *) echo "Unknown PROFILE=$PROFILE (use minimal or desktop)" >&2; exit 1 ;; \
    esac; \
    apt-get update; \
    cat $lists | grep -vE '^[[:space:]]*(#|$)' | xargs apt-get install -y; \
    if [ "$TOOLS" != none ]; then apt-get install -y $TOOLS; fi; \
    apt-get clean; \
    rm -rf /var/lib/apt/lists/* /tmp/packages-*.txt

# Default Kali credentials (kali/kali), same as official Kali images.
RUN groups=sudo; \
    for g in netdev bluetooth plugdev input video audio; do \
        getent group "$g" >/dev/null && groups="$groups,$g"; \
    done; \
    useradd -m -s "$(command -v zsh || echo /bin/bash)" -G "$groups" kali \
 && echo 'kali:kali' | chpasswd

COPY overlay/ /

# Every image would otherwise ship the same SSH host keys and machine-id.
RUN rm -f /etc/ssh/ssh_host_* \
 && : > /etc/machine-id \
 && rm -f /var/lib/dbus/machine-id \
 && systemctl enable kali-ps4-hostkeys.service NetworkManager.service ps4-hdmi-keepalive.service \
 && systemctl enable ssh.service \
 && if [ "$PROFILE" = desktop ]; then systemctl set-default graphical.target; \
    else systemctl set-default multi-user.target; fi
