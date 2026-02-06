# PW_ROC

This is a standalone repository containing the roc-toolkit PipeWire modules source code. While PipeWire includes these modules in its source tree, many popular operating systems ship PipeWire builds without them.

The purpose of this repository is to:
- Make it easier to deploy roc-sink and roc-source modules onto these systems
- Serve as a place to accommodate new features available in the `develop` and `master` branches of roc-toolkit

Tagged commits in this repository contain source code originally taken from the PipeWire version reflected in the tag.

## Build

```bash
meson setup build
meson compile -C build
meson install -C build
```
