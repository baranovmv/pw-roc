# PW_ROC

This is a standalone repository containing the roc-toolkit PipeWire modules source code. While PipeWire includes these modules in its source tree, many popular operating systems ship PipeWire builds without them.

The purpose of this repository is to:
- Make it easier to deploy roc-sink and roc-source modules onto these systems
- Serve as a place to accommodate new features available in the `develop` and `master` branches of roc-toolkit

Tagged commits in this repository contain source code originally taken from the PipeWire version reflected in the tag.

## Install build dependencies 

```bash
sudo apt install -y libpipewire-0.3-dev libspa-0.2-dev build-essential g++ pkg-config scons ragel gengetopt \
 libunwind-dev libsndfile-dev libtool intltool autoconf automake make cmake meson git
```
## Build

If you don't have roc-toolkit installed or you want to have more modern version of roc-toolkit then
your OS provide, you can link roc-toolkit statically. In that case add this option to meson setup command:
`meson setup build -Dforce-static-roc=true`

```bash
git clone https://github.com/baranovmv/pw-roc.git
git submodule update --init --recursive
meson setup build # it can much time as roc-toolkit will be built here
meson compile -C build
sudo meson install -C build
```

## Configure and reload

PipeWire expects config files in ~/.config/pipewire/

```bash
systemctl --user restart wireplumber pipewire
```