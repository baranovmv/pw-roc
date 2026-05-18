#!/usr/bin/env bash
#
# Run a command inside a CI Docker container.
#
# Usage:
#   ci/docker.sh <image> <command> [args...]
#
# Mounts the pw-roc source tree at /work and uses ci/docker/entrypoint.sh
# to start PipeWire + WirePlumber before executing the command.
set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "Usage: $0 <image> <command> [args...]" >&2
    exit 1
fi

IMAGE="$1"
shift

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

docker run --rm \
    -v "$REPO_DIR":/work \
    -w /work \
    -e ARTIFACT_NAME="${ARTIFACT_NAME:-}" \
    -e GITHUB_SHA="${GITHUB_SHA:-}" \
    --entrypoint /work/ci/docker/entrypoint.sh \
    "$IMAGE" \
    "$@"
