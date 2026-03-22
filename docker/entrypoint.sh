#!/usr/bin/env bash
# CI entrypoint: bring up PipeWire + WirePlumber, then run the given command.
set -euo pipefail

# ── 1. Runtime directory ──────────────────────────────────────────────────────
# Derive from the actual UID (not hardcoded) so the image works regardless
# of which UID useradd assigned to the 'ci' user.
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

# ── 2. D-Bus session bus ──────────────────────────────────────────────────────
# WirePlumber uses D-Bus for its portal/access policy and for finding the
# default metadata store.  Start a minimal session daemon on a Unix socket
# inside XDG_RUNTIME_DIR so it is cleaned up automatically on exit.
dbus-daemon \
    --session \
    --address="unix:path=${XDG_RUNTIME_DIR}/bus" \
    --nofork --nopidfile &
DBUS_PID=$!
export DBUS_SESSION_BUS_ADDRESS="unix:path=${XDG_RUNTIME_DIR}/bus"
# Give the daemon a moment to create the socket before clients connect.
sleep 0.2

# ── 3. PipeWire daemon ────────────────────────────────────────────────────────
# The default pipewire.conf already contains a support.node.driver object
# (Dummy-Driver) that supplies a graph clock when no real audio hardware is
# present, so no custom daemon config is needed.
pipewire &
PW_PID=$!

echo "Waiting for PipeWire socket…"
for i in $(seq 50); do
    [ -S "${XDG_RUNTIME_DIR}/pipewire-0" ] && break
    sleep 0.1
done
[ -S "${XDG_RUNTIME_DIR}/pipewire-0" ] \
    || { echo "ERROR: PipeWire socket never appeared"; exit 1; }
echo "PipeWire ready (pid ${PW_PID})"

# ── 4. WirePlumber session manager ────────────────────────────────────────────
# ~/.config/wireplumber/wireplumber.conf (installed by the Dockerfile) overrides
# the system config to load main.lua + policy.lua but NOT bluetooth.lua.
# The ALSA monitor inside main.lua finds no hardware and exits silently.
# policy.lua provides the stream-linking logic the tests rely on.
wireplumber &
WP_PID=$!

echo "Waiting for WirePlumber to join the PipeWire graph…"
for i in $(seq 50); do
    wpctl status 2>/dev/null | grep -q 'WirePlumber' && break
    sleep 0.2
done
wpctl status 2>/dev/null | grep -q 'WirePlumber' \
    || { echo "ERROR: WirePlumber never appeared in PipeWire graph"; exit 1; }

# Give WirePlumber one additional second to finish ingesting the initial
# graph state and arm its linking policy before the tests start.
sleep 1
echo "WirePlumber ready (pid ${WP_PID})"

# ── 5. Run command ────────────────────────────────────────────────────────────
"$@"
EXIT=$?

# ── Cleanup ───────────────────────────────────────────────────────────────────
kill "$WP_PID" "$PW_PID" "$DBUS_PID" 2>/dev/null || true
exit $EXIT
