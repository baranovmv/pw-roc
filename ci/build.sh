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

# Optional packaging step: when ARTIFACT_NAME is set (by the GitHub Actions
# workflow), stage the build outputs into build/dist/<name>/ and produce a
# tar.gz alongside it. Skipped silently for local invocations.
if [[ -n "${ARTIFACT_NAME:-}" ]]; then
    dist_root="/work/build/dist"
    stage="${dist_root}/${ARTIFACT_NAME}"
    rm -rf "${stage}" "${stage}.tar.gz"
    mkdir -p "${stage}/lib" "${stage}/include"

    cp /work/build/libpipewire-module-roc-sink.so   "${stage}/lib/"
    cp /work/build/libpipewire-module-roc-source.so "${stage}/lib/"
    cp /work/build/roc-toolkit/lib/libroc.a         "${stage}/lib/"
    cp -r /work/build/roc-toolkit/include/roc       "${stage}/include/"

    {
        echo "pw-roc build artifact"
        echo "artifact:   ${ARTIFACT_NAME}"
        echo "commit:     ${GITHUB_SHA:-unknown}"
        echo "built-at:   $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo
        echo "Contents:"
        echo "  lib/libpipewire-module-roc-sink.so"
        echo "  lib/libpipewire-module-roc-source.so"
        echo "  lib/libroc.a"
        echo "  include/roc/*.h"
    } > "${stage}/README"

    tar -C "${dist_root}" -czf "${stage}.tar.gz" "${ARTIFACT_NAME}"
    echo "Packaged ${stage}.tar.gz"
fi
