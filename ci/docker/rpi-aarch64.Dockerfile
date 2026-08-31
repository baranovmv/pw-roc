# syntax=docker/dockerfile:1-labs
# Cross-build pw-roc for Raspberry Pi OS Trixie (aarch64).
# (1-labs frontend required for `RUN --security=insecure`.)
#
# Stage 1 (native amd64): use rpi-image-gen to produce an arm64 RPi-OS-
# compatible Debian Trixie rootfs tarball pre-loaded with the build toolchain
# (pipewire/spa dev, meson, scons, ragel, gengetopt, autotools, ...).
#
# Stage 2 (--platform=linux/arm64, run via QEMU binfmt on the host): use that
# rootfs as the base, copy the pw-roc source tree, and run
# `meson compile -C build`. The resulting .so files are aarch64 ELF objects
# linked against the same libpipewire/libspa Raspberry Pi OS Trixie ships.
#
# Build context must be the repository root so that:
#   * 3rdparty/roc-toolkit (submodule) is available
#   * .rpi-image-gen/ (cloned upstream) is available
#   * ci/rpi-image-gen/ (this project's config + layer) is available
#
# Prerequisites:
#   git submodule update --init --recursive
#   docker run --privileged --rm tonistiigi/binfmt --install arm64
#
# Build (stage 1 needs CAP_SYS_ADMIN for mmdebstrap, hence
# `--allow security.insecure`):
#   docker buildx build --allow security.insecure \
#       -f ci/docker/rpi-aarch64.Dockerfile --load \
#       -t pw-roc:rpi-arm64 .
#
# Extract the built modules:
#   id=$(docker create pw-roc:rpi-arm64) && \
#   docker cp "$id":/work/build/libpipewire-module-roc-sink.so   ./ && \
#   docker cp "$id":/work/build/libpipewire-module-roc-source.so ./ && \
#   docker rm "$id"

# ─────────────────────────────────────────────────────────────────────────────
# Stage 1: build an arm64 RPi-OS-compatible rootfs tarball with rpi-image-gen.
# Always native to the build host to avoid running mmdebstrap under qemu.
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=$BUILDPLATFORM debian:trixie AS chroot-builder

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

# Install rpi-image-gen host dependencies. QEMU/binfmt for arm64 emulation is
# registered on the host kernel (via `tonistiigi/binfmt --install arm64`) and
# uses the F-flag, so the build container itself does not need qemu-user-
# static installed inside it. `sudo` is required by some rpi-image-gen helper
# scripts even when invoked as root.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        ca-certificates \
        git \
        sudo \
 && rm -rf /var/lib/apt/lists/*

# Non-root user: rpi-image-gen runs unprivileged and uses podman/uidmap.
RUN useradd -m -s /bin/bash -u 1000 builder \
 && echo 'builder ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/builder

WORKDIR /work
COPY --chown=builder:builder .rpi-image-gen     /work/.rpi-image-gen
COPY --chown=builder:builder ci/rpi-image-gen   /work/ci/rpi-image-gen

# Install base + build deps declared in .rpi-image-gen/depends.
# install_deps.sh requires /proc/sys/fs/binfmt_misc to be mounted (its
# dependency check refuses to proceed otherwise). We mount it inline with
# `--security=insecure`; the host kernel already has the module loaded via
# `tonistiigi/binfmt --install arm64`.
RUN --security=insecure \
    apt-get update \
 && (mountpoint -q /proc/sys/fs/binfmt_misc \
     || mount binfmt_misc -t binfmt_misc /proc/sys/fs/binfmt_misc) \
 && /work/.rpi-image-gen/install_deps.sh \
 && rm -rf /var/lib/apt/lists/*

USER builder

# rpi-image-gen needs CAP_SYS_ADMIN for mmdebstrap's mount namespaces; this
# requires `docker buildx --allow security.insecure`. binfmt_misc is mounted
# inline so the dependency check inside `rpi-image-gen build` passes; the
# host kernel must already have the module loaded (tonistiigi/binfmt).
#
# `customize50-cryptroot` is unconditionally run in every build but is a
# no-op for non-image targets and references an unbound variable under
# `set -eu`. Stub it out — we don't ship LUKS volumes from a tarball-only
# build.
RUN --security=insecure \
    sudo sh -c 'mountpoint -q /proc/sys/fs/binfmt_misc \
       || mount binfmt_misc -t binfmt_misc /proc/sys/fs/binfmt_misc' \
 && printf '%s\n' '#!/bin/sh' 'exit 0' \
        | sudo tee /work/.rpi-image-gen/scripts/bdebstrap/customize50-cryptroot \
        >/dev/null \
 && cd /work/.rpi-image-gen \
 && ./rpi-image-gen build \
        -S /work/ci/rpi-image-gen \
        -c pw-roc-arm64.yaml

# Extract the rootfs tarball to /rootfs so stage 2 can `COPY --from=...`
# directly (BuildKit doesn't expand tarballs in `COPY --from`).
USER root
RUN mkdir -p /rootfs \
 && tar -C /rootfs -xpf /work/.rpi-image-gen/work/pw-roc-arm64/rootfs.tgz \
 && rm -rf /work/.rpi-image-gen/work

# ─────────────────────────────────────────────────────────────────────────────
# Stage 2: cross-compile inside the arm64 rootfs (executed via QEMU binfmt).
# `--platform` is set on the buildx command line (linux/arm64); we use
# $TARGETPLATFORM here to silence the "constant flag value" lint.
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=$TARGETPLATFORM scratch AS cross-build

COPY --from=chroot-builder /rootfs/ /

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    CMAKE_POLICY_VERSION_MINIMUM=3.5

# Non-root CI user (PipeWire refuses to run as root). Mirrors ubuntu.Dockerfile.
RUN useradd -m -s /bin/bash ci \
 && mkdir -p /run/user/$(id -u ci) \
 && chown ci:ci /run/user/$(id -u ci) \
 && chmod 700 /run/user/$(id -u ci)

# Project sources (the rootfs ships with all build deps already installed by
# the pw-roc-builddeps layer in stage 1).
COPY --chown=ci:ci . /work
# Drop host-side tooling that has no business inside the cross rootfs.
RUN rm -rf /work/.rpi-image-gen /work/build

USER ci
WORKDIR /work

# Build pw-roc + bundled roc-toolkit. Same flags as the native ubuntu image.
RUN meson setup build -Dforce-static-roc=true \
 && meson compile -C build

ENTRYPOINT ["/work/ci/docker/entrypoint.sh"]
CMD ["meson", "test", "-C", "build", "--verbose"]
