#!/usr/bin/env bash
# CI entrypoint: bring up PipeWire + WirePlumber, then run the given command.
#
# Shared across all distro images under ci/docker/.  Designed for headless
# containers — no real audio hardware, dummy graph clock from pipewire's
# default support.node.driver.
set -euo pipefail

# ── 1. Runtime directory ─────────────────────────────────────────────────────
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

# ── 2. D-Bus session bus ─────────────────────────────────────────────────────
dbus-daemon \
    --session \
    --address="unix:path=${XDG_RUNTIME_DIR}/bus" \
    --nofork --nopidfile &
DBUS_PID=$!
export DBUS_SESSION_BUS_ADDRESS="unix:path=${XDG_RUNTIME_DIR}/bus"
sleep 0.2

# ── 3. PipeWire daemon ───────────────────────────────────────────────────────
pipewire &
PW_PID=$!

echo "Waiting for PipeWire socket…"
for _ in $(seq 50); do
    [ -S "${XDG_RUNTIME_DIR}/pipewire-0" ] && break
    sleep 0.1
done
[ -S "${XDG_RUNTIME_DIR}/pipewire-0" ] \
    || { echo "ERROR: PipeWire socket never appeared"; exit 1; }
echo "PipeWire ready (pid ${PW_PID})"

# ── 4. WirePlumber session manager ───────────────────────────────────────────
wireplumber &
WP_PID=$!

echo "Waiting for WirePlumber to join the PipeWire graph…"
for _ in $(seq 50); do
    wpctl status 2>/dev/null | grep -q 'WirePlumber' && break
    sleep 0.2
done
wpctl status 2>/dev/null | grep -q 'WirePlumber' \
    || { echo "ERROR: WirePlumber never appeared in PipeWire graph"; exit 1; }

sleep 1
echo "WirePlumber ready (pid ${WP_PID})"

# ── 5. Run command ───────────────────────────────────────────────────────────
"$@"
EXIT=$?

kill "$WP_PID" "$PW_PID" "$DBUS_PID" 2>/dev/null || true
wait "$WP_PID" "$PW_PID" "$DBUS_PID" 2>/dev/null || true
exit $EXIT
