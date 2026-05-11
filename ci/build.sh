#!/usr/bin/env bash
#
# Build and test pw-roc inside a CI container.
#
# This script is called by ci/docker.sh after PipeWire + WirePlumber
# have been started by the entrypoint.
set -euo pipefail

cd /work

# Detect system PipeWire module directory
PW_MODULE_DIR=$(pkg-config --variable=moduledir libpipewire-0.3 2>/dev/null || true)
if [[ -z "$PW_MODULE_DIR" ]]; then
    PW_MODULE_DIR=$(find /usr/lib -name "libpipewire-module-protocol-native.so" -printf '%h\n' -quit 2>/dev/null || true)
fi
export PIPEWIRE_MODULE_DIR="/work/build:${PW_MODULE_DIR}"

meson setup build -Dforce-static-roc=true
meson compile -C build
meson test -C build --verbose
