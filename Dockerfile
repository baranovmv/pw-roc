FROM ubuntu:latest

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
        # Build toolchain
        build-essential \
        g++ \
        cmake \
        meson \
        ninja-build \
        pkg-config \
        scons \
        python3 \
        # PipeWire and SPA development headers
        libpipewire-0.3-dev \
        libspa-0.2-dev \
        # PipeWire daemon and WirePlumber session manager
        pipewire \
        wireplumber \
        # D-Bus session bus (WirePlumber uses it for portal/policy)
        dbus \
    && rm -rf /var/lib/apt/lists/*

# PipeWire must not run as root.  Create a dedicated CI user and its
# XDG_RUNTIME_DIR while we still have root access to /run/user/.
RUN useradd -m ci \
 && mkdir -p /run/user/$(id -u ci) \
 && chown ci:ci /run/user/$(id -u ci) \
 && chmod 700 /run/user/$(id -u ci)

# Copy project source.
# The 3rdparty/roc-toolkit git submodule must be initialised before building
# the image: `git submodule update --init`.
COPY . /work
RUN chown -R ci:ci /work

# Install our slim WirePlumber config (omits Bluetooth — BlueZ is absent in a
# container and causes noisy D-Bus "service not found" errors).
RUN mkdir -p /home/ci/.config/wireplumber \
 && cp /work/docker/wireplumber.conf /home/ci/.config/wireplumber/wireplumber.conf \
 && chown -R ci:ci /home/ci/.config

USER ci
WORKDIR /work

ENTRYPOINT ["/work/docker/entrypoint.sh"]
