# Tools

Root-level tools serve the original iOS16 workspace. For newer versions, start
with the corresponding [source guide](../source/) and its matched local inputs.

## Boot

Boot helpers for the prepared local profile.

[boot-op13-wifi.py](boot-op13-wifi.py) · [boot-op13-play.py](boot-op13-play.py) · [boot-op13-live-usb.py](boot-op13-live-usb.py)

## Build

Kernel, graphics and packaging tools.

[build-xnu-macos.py](build-xnu-macos.py) · [build-mesa26-macos.py](build-mesa26-macos.py) · [prepare-op13-gles-mesa.py](prepare-op13-gles-mesa.py)

## Checks

Focused source and protocol regression checks.

[test-op13-slider.py](test-op13-slider.py) · [test-op13-battery-wire.py](test-op13-battery-wire.py) · [test-op13-display-commit.py](test-op13-display-commit.py)

Compiled artifacts and private inputs stay local. Follow the branch’s
[quickstart](../QUICKSTART.txt) before running a boot helper.
