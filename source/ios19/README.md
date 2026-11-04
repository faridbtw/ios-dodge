# iOS 19 on OnePlus 13

Tested with the user-provided **Solarium iOS 19.0 Internal / 23A191a, D84AP** root filesystem. The current local snapshot is `verified-slider-v6`. This branch contains source and sanitized results; Apple runtime files and generated boot images remain local. The physical phone is a OnePlus 13, regardless of its compatibility model name.

## Confirmed behavior

The user confirmed wake, touch, much smoother scrolling, wallpaper display/selection, battery/Island positions and all three alert-slider modes. The slider maps bottom to Ring, middle to Silent and top to Do Not Disturb. Live logs confirmed native ringer updates and successful Focus assertion take/release.

Two five-second display windows reached 52 and 59 presented FPS after native EDR service startup removed the observed five-second IPC delays. This does not establish 60/120 FPS across apps. Seven cores run iOS; firmware retains one core for USB diagnostics.

## Fixes preserved here

| Component | Change |
| :--- | :--- |
| Swift runtime | Eight class-data masks use the kernel's 38-bit address space. The patcher checks original instructions, refreshes ad-hoc cache page hashes and independently reads back the local APFS resource stream. |
| Bootstrap IPC | Explicit opt-in reply-port compatibility for pre-26 iOS bootstrap clients, scoped to their platform/SDK. Other IPC validation remains enabled. |
| XNU diagnostics | Guard an absent loaded-kext summary table and capture fatal exception metadata. |
| Display | Decode iOS 19's `0x514` framebuffer request and its `0x48` field shift; present native IOSurfaces instead of leaving the UEFI picture. |
| Adreno/Metal | Use the existing SM8750 `dadagpu=6` hardware startup and governor. Accept the legacy zero-argument AIR read-sampler constructor with its validated address-space-2 sampler ABI. Increment shader-cache semantics. |
| EDR | Start the inspected native `EDRServerStartService` in backboardd. It returned in 6292 µs in the recorded boot. |
| Native resources | Restore original loose Cryptex libraries/framework executables, UIKit artwork and native service definitions; PID 1 mounts the support overlays before native launchd. |
| Wallpaper | Inject PosterBoard; retry unsupported file-protection checks only on RAM storage and allow its optional purgeable hint there. Native store migration 59→61 then succeeds. No fixed-offset poster-role retry is enabled. |
| Battery | Enable the existing get-only OnePlus ADSP/PMIC GLINK driver with original local firmware. Readings reach native IORegistry and IOPowerSources. No charger SET commands are introduced. |
| Layout | Reuse guarded native UIKit status-bar allocation and physical-camera geometry; native battery view has a 22-point right inset. |
| Alert slider | Power/probe AK09970 in UEFI, reuse private factory calibration, enable the existing kernel sensor driver, consume slider input separately from power/volume keys, then call native iOS 19 ringer/Focus APIs. The ringer `clientType` is **unsigned int**, checked at runtime. Focus IPC runs off the UI queue. |
| Model name | Optional UEFI tool changes only two equal-length model-name strings per loader route to iPhone 15 Pro Max. Capability identifiers stay unchanged. |

The no-audio profile avoids synchronous unavailable-audio queries. Its muted-state getter reflects the actual slider position; audio output itself is not implemented.

## Source layout

- `patches/kernel-ios19.patch`: four-file delta against the kernel already in `../ios27/xnu-xnu-12377.121.6/`.
- `patches/metal2vulkan-legacy-sampler.patch`: translator, cache-version and authored regression-fixture delta against `../ios27/gpu/metal2vulkan/`. No captured guest AIR is included.
- `userland/inject-solarium19.c`: exact slider build's shared input, compatibility and wallpaper helper. The historical output name remains `libredwood26.dylib`.
- `compat/`: tested slider/layout, native EDR startup, battery probe and static PID1 support-overlay handoff.
- `libmini/`: the matching freestanding syscall shim used by that handoff.
- `config/`: injection configuration, custom battery-probe job, Swift patch plan and PAuth Rust target.
- `tools/build-compat.py`: portable compilation/signing of the native helpers using your local iPhoneOS SDK.
- `tools/boot-ios19.py`: profile-driven RAM-only runner with the conditional UEFI slider probe.
- `tools/workspace/`: original local-workspace recipes, adapted to read `IOS19_RAM_DIR`. They require the prepared workspace and other local inputs described below; they are not a complete image builder.

## Rebuilding

Make isolated copies of the two `source/ios27` bases outside this checkout, then apply each corresponding patch with `git apply` from its copy's root. Never overwrite the working iOS 16/27 trees. Kernel linking uses `../ios27/tools/build-kernel-macos.py` and the matching Dada dependencies, SDK, ath12k link object and local Focaltech header. Hardware firmware is deliberately absent.

Compile the native helpers from the repository root:

```sh
python3 source/ios19/tools/build-compat.py --output build/ios19-root --bootstrap
```

This requires a compatible installed iPhoneOS SDK and builds arm64e helpers with iOS 18 minimum-version metadata for the native iOS 19 runtime. The renderer retains its separate arm64e/PAuth Rust/C++ build. Apply the sampler patch to an isolated translator copy, retain the matching existing NativeMetal frontend/PM4 source, and use the local workspace renderer recipes. Original provider assets and dependencies remain required.

For existing Solarium workspaces, set `IOS19_RAM_DIR` to the prepared `oneplus13-ram` directory before running a `tools/workspace/` recipe. These retain the established sibling `OnePlus13-iOS-RAM` build/dependency layout. The Swift tool expects the copied `SWIFT-PATCH-PLAN.json`, selected stock source, boot-only tree and boot image. It asserts the expected old bytes and is not a generic patcher for other builds. Restoration tools stream exact files from the original local archive; they do not download or redistribute Apple files.

Keep the original source archive and stock selected image unchanged. Stage the compatibility outputs into a separate boot root, preserve native job/resource overlays, enable the native internal IDS prototype flag used in bring-up, include original ADSP firmware in the separate system RAM image and supply your device's private slider calibration locally. Build a matching APFS bootstrap, kernel payload and local preflight profile. The complete payload must fit the loader's 64 MiB limit and injection configuration must stay below 512 bytes. The tested slider payload is 61,731,032 bytes.

Profiles contain paths, sizes, SHA-256 hashes and matching boot arguments. Relevant settings are `cpus=7 bootcpu=1`, `ufs=0 ufs_write=0 var_store=0`, `op13_usb=1`, `ram_ephemeral=1`, `dadagpu=6`, `dadacpu=1`, `edr_server=1`, `op13_batt=4 battmgr=1`, `gl=0`, `slider=1 slider_power=1` and a locally supplied `slider_cal`. `gpu=0 adreno=0` disable older driver paths; the hardware renderer is selected by `dadagpu=6 metal=1`. Preserve the complete matching profile rather than constructing a new command line from this abbreviated list.

## Booting

Start Android, authorize USB debugging and connect the phone. Supply a prepared private profile and serial:

```sh
python3 source/ios19/tools/boot-ios19.py --profile /path/to/BOOT-PREFLIGHT.json --serial YOUR_SERIAL --check
python3 source/ios19/tools/boot-ios19.py --profile /path/to/BOOT-PREFLIGHT.json --serial YOUR_SERIAL
```

Python 3.11+ and Android platform tools are required. The check verifies all image hashes without contacting the phone. The normal run uses `fastboot boot`, stages matching images into RAM, probes/powers the slider when enabled, starts iOS and collects local logs. Never flash or erase. Keep USB connected through upload. Unplugging after startup leaves RAM iOS running; a reboot loses it.

To return to Android with responsive firmware USB, use `fastboot -s xnuloader oem reboot`. Use a cold restart before another GPU/DSP session; do not reuse an `oem stop` session. If USB is unresponsive, hold Power + Volume Up and let Android start.

## Remaining limitations

- Status-bar clock is still absent despite an enabled native time producer. No replacement clock label is claimed.
- DSP raw SOC reads 100% versus an earlier Android UI baseline of 90%; percentage accuracy/smoothing is not validated. Charging disconnect/reconnect has not yet been retested on this iOS 19 profile.
- Home Screen → Edit → Customize for icon colors can respring. Wallpaper selection itself is confirmed working.
- Wi-Fi and audio integration are disabled. Many ordinary apps are only placeholders in this internal source image; broad app compatibility is untested.
- The iPhone 15 Pro Max display-name UEFI boots, but its Settings name was not separately confirmed.

The focused sampler signature test and authored legacy cross-block regression passed in the original workspace. Full upstream CI was not run, and the Metal-assembler leg of an existing mixed test was unavailable. See [sanitized validation](../../validation/ios19/STATUS.json). Apple images, firmware/calibration, guest shaders, raw logs and all compiled artifacts are excluded from this branch.
