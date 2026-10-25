# ios-dodge

Experimental XNU/iOS RAM boot work for **OnePlus 13 (`dodge`, Qualcomm SM8750)**.

This repository preserves the port source, host tools and current validation summaries. Supply your own compatible iOS system image, SDKs and device firmware. Binary boot bundles and firmware dumps are not included.

## Current status

Verified on the connected OnePlus 13 on 25 October 2026:

- iOS starts and touch works.
- Eight CPU cores are enabled; the earlier CPU workload test exercised all eight.
- The original CoreAnimation software compositor (`gl=0`) is noticeably faster in the user's testing than Mesa softpipe.
- Wallpaper, icons and readable text are visible with the full wallpaper service configuration.
- Battery percentage and charging indication work. The user confirmed the charging bolt disappears when unplugged and returns after reconnecting.
- The final timed battery diagnostic recorded 39 fresh samples and recovered Android by cold reset.
- The alert slider works: bottom rings, middle is silent, top enables Do Not Disturb. All physical positions were captured, and the user confirmed Silent Mode and Do Not Disturb in the eight-core interactive build.
- The UEFI loader uses a centered turtle logo, larger live log text and upload progress. Logs flow through the full screen behind the logo. A cached text surface keeps the turtle visible while logs scroll.
- UEFI USB reconnect works across two physical unplug/replug tests without rebooting; commands, uploads and log downloads recovered in the same loader session.

Adreno 830 hardware acceleration is unfinished. GPU command submission probes are experimental and disabled in the interactive configuration. Smooth 120 FPS operation is not established. Battery health, design capacity and cycle metadata remain unverified and are not published by the new bridge.

## Layout

- `source/xnu-build-snapshot/xnu-8796.121.2/`: active XNU source snapshot, including the OnePlus battery transport and CPU/GPU work.
- `source/hackios-xnu-kernel/`: upstream kernel project and host build support.
- `source/hackios-xnu-userland/`: injected libraries, UI support, APFS tools and Mesa integration.
- `source/touch-runtime-v15/`: RAM loader and touch handoff source.
- `assets/uefi/`: turtle artwork, native boot bitmap and generation prompt.
- `tools/`: native macOS build, payload packaging and RAM diagnostic tools.
- `validation/`: selected build and status summaries; device logs and raw dumps are excluded.
- `diagnostics/20261025-battery/oplus_hal_adsp.[ch]`: pinned vendor ABI reference used by the battery wire tests, retaining its original license notices.

The optional reduced-effects injection is an experiment. Removing the dock background alone did not solve the lag. The selected interactive configuration uses the original compositor without that injection.

## RAM boot

Use matching, locally prepared boot artifacts. The Android bootloader identifies as `sun`; the temporary RAM loader identifies as `xnuloader`.

```sh
adb -s YOUR_SERIAL reboot bootloader
fastboot -s YOUR_SERIAL boot YOUR_MATCHING_DODGE_LOADER.img
fastboot -s xnuloader stage YOUR_MATCHING_PAYLOAD.bin
fastboot -s xnuloader oem ramdisk
fastboot -s xnuloader stage system.apfs
fastboot -s xnuloader oem sm8750-touch-arm
fastboot -s xnuloader oem run 0
```

These commands boot and stage into RAM. **Do not flash or erase phone partitions.** The interactive build has no automatic return timer. Once ADSP is running, the new driver uses a cold reset on kernel return so the loader cannot reuse DSP-owned memory.

See [QUICKSTART.txt](QUICKSTART.txt) for the current local build configuration and [SOURCE-PROVENANCE.md](SOURCE-PROVENANCE.md) for source revisions and artifact hashes.

## Building

This is a development snapshot, not a standalone boot image release. The build tools still require local inputs omitted from this repository: the compatible iOS 16.5 / 20F66 system image, original payload template and matching loader, an iOS SDK, native build dependencies, and captured device firmware. Generated FocalTech firmware headers and vendor binary resources are also omitted; restore required inputs from their original sources before building.

The working native kernel build uses:

```sh
python3 tools/build-xnu-macos.py export
python3 tools/build-xnu-macos.py build
```

With the local dependencies and baseline inputs restored, `tools/make-op13-test-payload.py --help` describes diagnostic and interactive payload options. The selected interactive profile is eight cores, `gl=0`, `fb_hz=60`, no benchmarks and `--battery-live` with matching device firmware. The older `adsp` driver remains disabled; `op13_batt=4` selects the new live battery transport.

The slider transport and position checks run without phone images or firmware:

```sh
python3 tools/test-op13-slider.py
```

An optional `--dtb LOCAL_ANDROID.dtb` also checks the sensor address and GPIOs against your locally captured device tree. See the alert-slider section in `QUICKSTART.txt` for its matching loader, local calibration and payload workflow.

The battery protocol checks can be run independently:

```sh
python3 tools/test-op13-battery-wire.py
```

Tests that use device firmware or GPU vendor snapshots require those local inputs as well.

The UEFI transport and display checks run without a phone or system image:

```sh
python3 tools/test-op13-usb-reconnect.py
python3 tools/test-op13-turtle-overlay.py
```

The tests exercise the actual loader code under AddressSanitizer and UndefinedBehaviorSanitizer. The overlay test checks that logs follow a normal full-screen console while the turtle remains visible between characters and scrolls. The transport test covers cancelled transfers, stale receives, failed-send event polling, kernel-side wait timeouts and repeated cable cycles. See the UEFI section of `QUICKSTART.txt` for packaging the current source with your own matching baseline inputs. The `validation/uefi/` summaries identify the phone-tested build; generated boot images remain local.

## Attribution and licenses

Built on HackiOS kernel/userland, Apple XNU, Project-Silicium and Qualcomm/OnePlus platform work. The modern ADSP transport was adapted from [hxfuxyy/ios-macan](https://github.com/hxfuxyy/ios-macan) for Dodge's own memory map and PMIC protocol.

The slider event/Focus path was adapted from [haxi0/iphont-xnu-userland](https://github.com/haxi0/iphont-xnu-userland) and the [OnePlus 8 kernel branch](https://github.com/haxi0/iphont-xnu-kernel/tree/instantnoodle), with the AK09970 sensor and power path adapted for Dodge.

The root [LICENSE](LICENSE) covers original project material as stated in that file. XNU and kernel-derived code retain APSL and other original file notices; userland and vendor reference files retain their respective licenses. This repository does not relicense third-party code.
