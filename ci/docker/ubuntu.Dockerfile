# syntax=docker/dockerfile:1.6 #
# CI image for pw-roc — Ubuntu (latest).
#
# Goals:
#   * Provide a reproducible environment for CI builds and tests.
#   * Compile the project during image build so the resulting image already
#     contains the built modules (binary preparation).
#   * Run a PipeWire + WirePlumber stack on container start so the modules
#     can be loaded for integration tests.
#
# Build context must be the repository root (so 3rdparty/roc-toolkit is
# available).  The roc-toolkit submodule must be initialised before build:
#   git submodule update --init --recursive
#
# Build:
#   docker build -f ci/docker/ubuntu.Dockerfile -t pw-roc:ubuntu .
#
# Run tests / commands (mount fresh sources optional):
#   docker run --rm pw-roc:ubuntu meson test -C /work/build --verbose
#   docker run --rm -v "$PWD":/work pw-roc:ubuntu meson compile -C /work/build

FROM ubuntu:latest

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

# ── Build + runtime packages ─────────────────────────────────────────────────
# Build toolchain: meson/ninja for this project, scons + g++ for the bundled
# roc-toolkit.  PipeWire/SPA dev headers for compilation; pipewire +
# wireplumber + dbus for running the daemons during tests.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        # Build toolchain
        build-essential \
        g++ \
        gdb \
        cmake \
        meson \
        ninja-build \
        pkg-config \
        scons \
        ragel \
        gengetopt \
        python3 \
        git \
        ca-certificates \
        # roc-toolkit optional deps (kept off via scons flags but headers help)
        libsndfile1-dev \
        libtool \
        intltool \
        autoconf \
        automake \
        # PipeWire / SPA development headers
        libpipewire-0.3-dev \
        libspa-0.2-dev \
        # Runtime: PipeWire daemon, WirePlumber session manager, D-Bus
        pipewire \
        pipewire-bin \
        wireplumber \
        dbus \
 && rm -rf /var/lib/apt/lists/*

# ── Non-root CI user ─────────────────────────────────────────────────────────
# PipeWire refuses to run as root.  Provision an XDG_RUNTIME_DIR while we
# still have privileges.
RUN useradd -m -s /bin/bash ci \
 && mkdir -p /run/user/$(id -u ci) \
 && chown ci:ci /run/user/$(id -u ci) \
 && chmod 700 /run/user/$(id -u ci)

# ── Project sources ──────────────────────────────────────────────────────────
COPY --chown=ci:ci . /work

USER ci
WORKDIR /work

# ── First compile (binary preparation) ───────────────────────────────────────
# Build with the bundled roc-toolkit so the image is self-contained and does
# not depend on whatever (often missing) roc package the distro provides.
#
# CMAKE_POLICY_VERSION_MINIMUM=3.5 is needed because Ubuntu ships CMake 4.x,
# which dropped compatibility with `cmake_minimum_required(VERSION < 3.5)`,
# while one of roc-toolkit's vendored deps (openfec) still declares 2.8.12.
ENV CMAKE_POLICY_VERSION_MINIMUM=3.5
RUN meson setup build -Dforce-static-roc=true \
 && meson compile -C build

ENTRYPOINT ["/work/ci/docker/entrypoint.sh"]
CMD ["meson", "test", "-C", "build", "--verbose"]
