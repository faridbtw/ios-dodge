# iOS 27.2 OnePlus 13 source snapshot

This branch preserves the experimental iOS 27.2 (24B119) work separately from the iOS 16 baseline. It is a source snapshot, not a ready-to-boot distribution. Apple system images, SDKs, dyld caches, proprietary firmware, calibration, kernels, payloads and raw device logs are excluded. Obtain local inputs separately; never commit system.apfs.

## Confirmed results

- iOS reaches the lock/home screen with touch and seven iOS cores. Firmware handles USB on the remaining core.
- Rebuilt arm64e Turnip/Vulkan and NativeMetal probes passed with zero failures. UI performance improved after starting the missing native EDR and InputUI services; 60/120 FPS is not established.
- Hardware battery percentage and live charging/unplug/reconnect reporting work through ADSP/PMIC GLINK and native IOPowerSources.
- Wallpaper displays after restoring PosterBoard and enabling the real Metal providers.
- Dynamic Island position and the latest battery placement were confirmed by the user. The clock remains absent.

## Open issues and rejected experiments

The wallpaper picker still fails or causes memory pressure/resprings. Black bands can appear at startup or during animation. Blank widgets remain. A CPU governor trial was rejected for worse responsiveness; it is not selected. Wi-Fi is disabled in this iOS27 profile: iOS16 Wi-Fi success does not establish iOS27 Wi-Fi support. Audio and Voice Memos are unverified; a freeze was reported. A later stop captured a shutdown panic; causation is unresolved. The native clock-timer restart did not restore the time. No replacement clock overlay has been implemented.

## Layout

- `xnu-xnu-12377.121.6/`: exact current XNU source snapshot, excluding the proprietary Focaltech firmware header. Source licenses are retained.
- `xnuloader/`: Project Boot loader source with OnePlus hardware support. The selected device build retains the existing turtle branding and serial configuration.
- `userland/src/`: iOS27 compatibility and runtime sources.
- `gpu/`: adapted NativeMetal, PM4 and Rust bridge sources, pinned origins in UPSTREAM.json; Mesa changes are a patch plus added files. Large validation corpora/build caches are excluded.
- `compat/edr-start.c`: native EDR service startup.
- `compat/wallpaper-queue.m`: bounded native wallpaper preview work.
- `compat/battery-probe.c`: native hardware/IOKit/IOPS verification helper.
- `compat/status-layout.m`: latest tested layout helper, including the confirmed Island and battery geometry; native clock startup remains ineffective.
- `tools/`: kernel build/validation and RAM boot helpers. Some paths retain the original local workspace assumptions and require adjustment.

## Build inputs and RAM boot

The kernel uses Apple's xnu-12377.121.6 and libpthread-539.100.4 sources, a compatible iPhoneOS SDK, Dada dependencies and the ath12k object required to resolve link symbols even when Wi-Fi is disabled. The missing Focaltech header is a local build input for an unused driver branch; never add firmware to this repository. Kernel linking on macOS uses `tools/build-kernel-macos.py`; run its help for source/work/dependency options. GPU origins are pinned in UPSTREAM.json. The exact active frontend is included, with conservative advisory working-set sizes and the original faster mirror path. Rejected fence/cache/heap experiments are not presented as fixes.

Native compatibility helpers target arm64e-apple-ios27.2 and are ad-hoc signed. status-layout.m links Foundation, UIKit and CoreGraphics. Apply the helper through the RAM root's inject.conf, preserving the other libraries; keep that configuration below the kernel's 512-byte limit. Build/package scripts still require local inputs and are not a one-command release build.

The selected local profile is status-layout-v4, using the verified battery-v1 kernel. It uploads the system image to RAM and uses `fastboot boot` only. Never flash or erase partitions. Capture live diagnostics before recovery. Retained ADSP/GPU resources require a cold reset before another run; `oem stop` has exposed a shutdown panic. The current loader supports `fastboot -s xnuloader oem reboot` as a cold reset without partition writes. Do not reuse a stopped GPU/DSP session in place.

See ../../validation/ios27/STATUS.json for the latest verified state and PERFORMANCE.md for measured trials.
