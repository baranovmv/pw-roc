# CI Docker images

Per-distro Dockerfiles for building and testing `pw-roc`. Each image:

1. Installs the build toolchain and PipeWire/SPA development headers.
2. Installs `pipewire`, `wireplumber`, and `dbus` for running an integration
   stack inside the container.
3. Copies the repository (build context = repo root) and compiles the modules
   with the bundled `roc-toolkit` (`-Dforce-static-roc=true`) so the image
   ships ready-to-use binaries.
4. On `docker run`, the shared `entrypoint.sh` brings up D-Bus, PipeWire, and
   WirePlumber, then `exec`s whatever command was passed (default:
   `meson test -C build --verbose`).

## Layout

```
ci/docker/
├── entrypoint.sh         # shared: starts dbus + pipewire + wireplumber
├── wireplumber.conf      # shared: headless WP config (no bluetooth)
├── ubuntu.Dockerfile     # ubuntu:latest
├── debian.Dockerfile     # (planned)
├── arch.Dockerfile       # (planned)
├── rpi-aarch64.Dockerfile    # Raspberry Pi OS Trixie / arm64 cross-build
└── raspios-armhf.Dockerfile  # (planned)
```

## Build

From the repository root (the build context **must** be the repo root):

```bash
git submodule update --init --recursive
docker build -f ci/docker/ubuntu.Dockerfile -t pw-roc:ubuntu .
```

## Run tests

```bash
docker run --rm pw-roc:ubuntu
# explicit:
docker run --rm pw-roc:ubuntu meson test -C build --verbose
```

## Run an interactive shell

```bash
docker run --rm -it --entrypoint bash pw-roc:ubuntu
```

## Rebuild against live sources

Mount the working tree over `/work` to recompile against your current
checkout instead of the snapshot baked into the image:

```bash
docker run --rm -v "$PWD":/work pw-roc:ubuntu \
    sh -c "meson setup build -Dforce-static-roc=true --wipe && \
           meson compile -C build && \
           meson test -C build --verbose"
```

## Cross-arch: Raspberry Pi OS Trixie (arm64)

`rpi-aarch64.Dockerfile` cross-builds the modules against the exact
`libpipewire-0.3` / `libspa-0.2` versions that Raspberry Pi OS Trixie
ships. Stage 1 invokes [rpi-image-gen](https://github.com/raspberrypi/rpi-image-gen)
(submodule expected at `.rpi-image-gen/`) to produce an arm64 rootfs
tarball preloaded with the build toolchain; stage 2 runs under QEMU
binfmt and compiles `pw-roc` + the bundled `roc-toolkit` inside that
rootfs.

Prerequisites:

```bash
git submodule update --init --recursive            # pulls roc-toolkit
git clone https://github.com/raspberrypi/rpi-image-gen.git .rpi-image-gen
docker run --privileged --rm tonistiigi/binfmt --install arm64
```

`mmdebstrap` (used by rpi-image-gen) needs `CAP_SYS_ADMIN`, which under
BuildKit is granted via the `security.insecure` entitlement:

```bash
docker buildx create --use --name pw-roc-builder \
    --buildkitd-flags '--allow-insecure-entitlement security.insecure'
docker buildx build --allow security.insecure \
    --platform linux/arm64 \
    -f ci/docker/rpi-aarch64.Dockerfile --load \
    -t pw-roc:rpi-arm64 .
```

Extract the built modules:

```bash
id=$(docker create pw-roc:rpi-arm64)
docker cp "$id":/work/build/libpipewire-module-roc-sink.so   ./
docker cp "$id":/work/build/libpipewire-module-roc-source.so ./
docker rm "$id"
file ./libpipewire-module-roc-sink.so
# → ELF 64-bit LSB shared object, ARM aarch64, ...
```

The default `CMD` runs the integration test suite under QEMU just like
the native `ubuntu` image:

```bash
docker run --rm --platform linux/arm64 pw-roc:rpi-arm64
# explicit:
docker run --rm --platform linux/arm64 pw-roc:rpi-arm64 \
    meson test -C build --verbose
```

Tests have been observed to pass under `qemu-user-static`, but emulated
PipeWire is timing-sensitive — if a run is flaky, re-run on real
Raspberry Pi hardware after copying the modules into
`/usr/lib/aarch64-linux-gnu/pipewire-0.3/`.

### armhf (planned)

The armhf variant will follow the same recipe with the corresponding
`debian-trixie-armhf-*` rpi-image-gen layer once upstream provides one.
