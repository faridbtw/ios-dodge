# ios-dodge

Experimental iOS16.5 /20F66 RAM port for **OnePlus13 (`dodge`, SM8750)**. This private repository contains source, build/boot tools and sanitized validation summaries. Supply your own SDK, compatible system image, firmware and matching local boot templates.

## Current phone-tested fixes

- Adreno830 hardware OpenGL ES3.2 rendering through Mesa26.2.5/Freedreno. Metal remains disabled.
- General scrolling is mostly tear-free: compose into off-screen RAM, then copy the complete frame after the panel TE signal. A small horizontal split can still occur when dismissing apps; the user considers it minor. Sampled copies took1.1–2.9ms within the observed8.29ms panel refresh period. This does not establish120FPS application rendering.
- Wallpaper selection works through Settings. Native catalog services are enabled on demand; a scoped CoreGraphics preview replaces a Metal-only capture that crashed PosterBoard. Lock-screen long-press selection remains disabled because its hosted scene renders black.
- Live battery percentage/charging updates and bottom ring / middle silent / top Do Not Disturb slider behavior are retained.
- Seven iOS cores run while CPU0 serves firmware USB diagnostics. The earlier eight-core software-renderer profile remains a fallback.
- Centered turtle, larger full-screen logs and USB reconnect code are retained. The v13 UEFI retains v12 support and accepts `fastboot -s xnuloader reboot` while iOS owns touch; Android startup completed in22.7seconds during testing.
- Screen brightness works in Settings and Control Center through native DCS51 panel commands. The scoped Control Center availability hook enables its slider.
- Flashlight on/off and brightness levels work through the native four-channel PM8550 driver, with a30-second activation cutoff.
- Sileo2.5.1 opens with a RAM-backed Procursus/ElleKit bootstrap. Package downloads and arbitrary tweaks remain unverified.
- Power and both volume buttons were user-confirmed after restoring the saved brightness build; the experimental tearing candidate remains unselected.

Wi-Fi and sound remain unfinished; sound work is paused. Long-term stability, full sensor/camera/radio support and120FPS need further validation. See [BUGLIST.txt](BUGLIST.txt) for remaining issues and [STATUS.json](STATUS.json) for the current build and validation summary.

## Source layout

- `source/xnu-build-snapshot/xnu-8796.121.2/`: current kernel, paired Adreno830/GPU user client, device-tree concurrency fix, battery/slider support and shadow/TE presentation.
- `source/touch-runtime-v15/`: current turtle/live-reset UEFI loader source.
- `source/hackios-xnu-userland/`: injected UI libraries, matching GLES/OSMesa bridge and historical Mesa integration sources.
- `source/mesa26-op13/`: consolidated source patch and changed-file hash manifest for the paired Mesa26.2.5 runtime.
- `source/wallpaper-preview/`: scoped preview helper and its build instructions.
- `source/screen-brightness/`: brightness agent, scoped Control Center hook, native DSI tests and feature rebuild instructions.
- `tools/`: host build, packaging, RAM boot and regression checks.
- `validation/`: sanitized feature results and artifact identities. Raw logs and device calibration are excluded.

The active kernel uses ordinary source paths. Historical `files/` overlays are retained for provenance and are not the current build input.

## Build and RAM boot

Restore the local dependencies described in [SOURCE-PROVENANCE.md](SOURCE-PROVENANCE.md), including iPhoneOS16.5 SDK, XNU dependencies, firmware inputs and matching baseline image/payload templates. Generated firmware headers are deliberately omitted. This is a source snapshot, not a self-contained binary release.

```sh
python3 tools/build-xnu-macos.py export
python3 tools/build-xnu-macos.py libs
python3 tools/build-xnu-macos.py build
```

The harness accepts `--source`, `--work` and `--deps` for isolated builds. Its default kernel source is the current tree above.

For the paired Mesa source, supply the release tarball identified by the manifest:

```sh
python3 tools/prepare-op13-gles-mesa.py --archive YOUR_MESA_26_2_5_TAR_XZ
OP13_IOS_SDK=YOUR_IPHONEOS16_5_SDK python3 tools/build-mesa26-macos.py
python3 tools/build-op13-gles-bridge.py --mesa YOUR_BUILT_OSMESA_DYLIB --sdk YOUR_IPHONEOS16_5_SDK
```

Use the consolidated patch once; do not then reapply the historical Mesa patches. The source patch reconstructs all59 changed/new donor text source files. Native kernel and GLES bridge compilation were checked separately from the earlier phone-tested artifacts; bit-for-bit reproduction of historical binaries is not claimed.

[QUICKSTART.txt](QUICKSTART.txt) identifies the selected local Sileo/flashlight/brightness payload and v13 UEFI, RAM boot commands and software-renderer fallback. [Feature rebuild instructions](source/screen-brightness/README.txt) describe preparing the separate Sileo system image and adding the native brightness helpers. Packaging helpers require the omitted local templates/artifacts; `experimental/` paths are local work outputs. `system.apfs`, boot images, firmware, SDKs, calibration and raw logs are never uploaded.

**RAM only: use `fastboot boot` and temporary staging. Never flash or erase phone partitions.** The GPU retains protected memory until cold reset; do not reuse its loader session through `oem stop`.

## Checks

```sh
python3 tools/test-op13-dsi-brightness.py
python3 tools/test-op13-display-commit.py
python3 tools/test-op13-live-reset.py
python3 tools/test-op13-usb-reconnect.py
python3 tools/test-op13-turtle-overlay.py
python3 tools/test-op13-slider.py
python3 tools/test-op13-battery-wire.py
```

The display/reset tests exercise actual implementation code under AddressSanitizer/UndefinedBehaviorSanitizer. Other listed checks cover USB reconnection, console/turtle memory guards, slider bus/position handling and battery protocol boundaries. Tests requiring device firmware or old GPU diagnostic inputs remain separate.

## Attribution and licenses

Built on HackiOS/OOSEX GANG, Apple XNU, Project-Silicium, Mesa/Freedreno and Qualcomm/OnePlus platform work. The paired GPU/GLES source was recovered from the user-supplied OnePlus13 GLESv3 archive, derived from [iola1999/hackios-xnu-kernel](https://github.com/iola1999/hackios-xnu-kernel/tree/k90-port) and [iola1999/hackios-xnu-userland](https://github.com/iola1999/hackios-xnu-userland/tree/k90-port). See [SOURCE-PROVENANCE.md](SOURCE-PROVENANCE.md) for pins and integration boundaries.

Battery transport references [ios-macan](https://github.com/hxfuxyy/ios-macan). Slider event/Focus behavior references [haxi0's OnePlus8 kernel](https://github.com/haxi0/iphont-xnu-kernel/tree/instantnoodle) and [userland](https://github.com/haxi0/iphont-xnu-userland), adapted to Dodge's AK09970 hardware.

Original source/license notices remain in place. The root [LICENSE](LICENSE) covers original project material; XNU/APSL, upstream MIT and vendor reference notices retain their respective scope. Third-party code is not relicensed by this update.
