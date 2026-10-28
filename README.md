<p align="center">
  <img src="assets/readme/banner.svg" width="1120" alt="ios-dodge — an experimental iOS port for OnePlus 13, with the project's turtle mascot">
</p>

<p align="center">
  <strong>OnePlus 13</strong> &nbsp; / &nbsp; <strong>iOS 16.5 · 20F66</strong> &nbsp; / &nbsp; <strong>RAM boot</strong>
</p>

<p align="center">
  <a href="QUICKSTART.txt">Quick start</a> &nbsp; · &nbsp;
  <a href="#feature-status">Feature status</a> &nbsp; · &nbsp;
  <a href="BUGLIST.txt">Bug list</a> &nbsp; · &nbsp;
  <a href="source/screen-brightness/README.txt">Build guide</a> &nbsp; · &nbsp;
  <a href="SOURCE-PROVENANCE.md">Sources &amp; credits</a>
</p>

---

An experimental iOS port for **OnePlus 13 (`dodge`, SM8750)**, with Adreno graphics, a custom UEFI and native hardware adaptations. This private repository contains the source, build tools and sanitized results from testing on the phone.

> **RAM only.** Use `fastboot boot` and temporary staging. Never flash or erase phone partitions. System images, firmware, SDKs, device calibration and raw logs stay local.

## Feature status

Status reflects the selected brightness build, last validated **29 October 2026**. Detailed evidence lives in [validation/](validation/); build identities are recorded in [STATUS.json](STATUS.json).

| Feature | Status | What works |
| :--- | :---: | :--- |
| **iOS & touch** | Working | Interactive iOS, with seven cores running iOS and CPU0 serving firmware USB. |
| **Adreno 830** | Working | Hardware OpenGL ES 3.2 through Mesa 26.2.5 / Freedreno. |
| **Screen brightness** | Working | Settings and Control Center sliders control the panel directly. |
| **Battery** | Working | Live percentage, charging state and unplug/reconnect updates. |
| **Alert slider** | Working | Bottom: ring · Middle: silent · Top: Do Not Disturb. |
| **Power & volume** | Working | Power, Volume Up and Volume Down confirmed in the selected build. |
| **Flashlight** | Working | On/off and brightness levels; automatic cutoff after 30 seconds per activation. |
| **Wallpapers** | Partial | Selection through Settings works; lock-screen long-press selection stays disabled. |
| **Sileo 2.5.1** | Working | Opens with a RAM-backed Procursus / ElleKit bootstrap. Downloads and arbitrary tweaks remain unverified. |
| **UEFI & USB** | Working | Centered turtle, larger logs, cable reconnect and live reboot to Android. |

### Still in progress

| Area | Current state |
| :--- | :--- |
| **Wi-Fi** | Connection and traffic are unfinished. |
| **Sound** | Speaker output is unresolved; work is paused. |
| **Metal & 120 FPS** | Metal is disabled. Application rendering at 120 FPS is unverified; FPS work is stopped. |
| **Display polish** | Scrolling is mostly tear-free. An occasional small split when dismissing apps remains a minor issue. |
| **Long-term stability** | Sustained load, broader sensors/cameras/radios and arbitrary tweak compatibility need validation. |

The experimental tearing candidate is **not selected**. The saved brightness build restored working buttons. See [BUGLIST.txt](BUGLIST.txt) for the full record.

## Get started

### 1. Prepare your local files

Supply your own compatible system image, SDK, firmware and matching boot templates. This is a source snapshot; generated OS and boot images are not included.

| Selected local artifact | Path |
| :--- | :--- |
| iOS UEFI | `experimental/uefi-turtle/screen-brightness-v13/dodge-ios-uefi.img` |
| iOS payload | `experimental/screen-brightness/control-v2/payload.bin` |
| System RAM image | `experimental/sileo/v1/system-sileo.apfs` |

These are **local output paths**, not repository downloads. [QUICKSTART.txt](QUICKSTART.txt) describes the matching artifacts and fallback build. For rebuilding, start with [dependencies and provenance](SOURCE-PROVENANCE.md) and the [Sileo, flashlight and brightness guide](source/screen-brightness/README.txt).

### 2. Check the package

Run from the repository directory. This validates the local artifacts without contacting the phone:

```sh
python3 tools/boot-op13-gles-v3.py \
  --payload experimental/screen-brightness/control-v2/payload.bin \
  --system experimental/sileo/v1/system-sileo.apfs \
  --check
```

### 3. Boot from Android

Connect the phone with Android fully started. Replace `YOUR_SERIAL` with its serial, and choose a **new** diagnostics output directory each time:

```sh
python3 tools/boot-op13-gles-v3.py \
  --payload experimental/screen-brightness/control-v2/payload.bin \
  --system experimental/sileo/v1/system-sileo.apfs \
  --serial YOUR_SERIAL \
  --output diagnostics/ios-next
```

The helper checks hashes, boots UEFI into RAM, prepares brightness and touch, uploads the system and starts iOS. It leaves iOS running and saves three live log captures.

**Return to Android** from the selected seven-core build:

```sh
fastboot -s xnuloader reboot
```

Cold-reset before another GPU session. The GPU retains protected memory; do not reuse the session through `oem stop`. Firmware USB provides diagnostics and reboot, rather than an iOS SSH or filesystem shell. Sileo packages and settings in this RAM profile reset after a cold reboot.

## Explore the source

| Directory | Contents |
| :--- | :--- |
| [Kernel](source/xnu-build-snapshot/xnu-8796.121.2/) | XNU, Adreno service, battery, slider, flashlight and display adaptations. |
| [UEFI](source/touch-runtime-v15/) | Console, USB reconnect, live reset and panel brightness preparation. |
| [Userland](source/hackios-xnu-userland/) | Injected UI libraries and the GLES / OSMesa bridge. |
| [Mesa](source/mesa26-op13/) | Consolidated Mesa 26.2.5 patch and changed-file hash manifest. |
| [Brightness](source/screen-brightness/) | Screen agent, scoped Control Center hook, DSI tests and feature build guide. |
| [Wallpaper preview](source/wallpaper-preview/) | Scoped CoreGraphics preview helper. |
| [Tools](tools/) | Build, packaging, temporary boot and regression checks. |
| [Validation](validation/) | Sanitized test results and artifact identities. |

The active kernel builds from its ordinary source paths. Historical `files/` overlays remain for provenance.

## Development

<details>
<summary><strong>Build the kernel and graphics bridge</strong></summary>

Restore the local dependencies from [SOURCE-PROVENANCE.md](SOURCE-PROVENANCE.md), then build:

```sh
python3 tools/build-xnu-macos.py export
python3 tools/build-xnu-macos.py libs
python3 tools/build-xnu-macos.py build
```

The harness accepts `--source`, `--work` and `--deps`. Use a separate work directory when switching source checkouts: generated dependency files keep absolute source paths.

For Mesa, supply the release archive identified by the manifest:

```sh
python3 tools/prepare-op13-gles-mesa.py --archive YOUR_MESA_26_2_5_TAR_XZ
OP13_IOS_SDK=YOUR_IPHONEOS16_5_SDK python3 tools/build-mesa26-macos.py
python3 tools/build-op13-gles-bridge.py \
  --mesa YOUR_BUILT_OSMESA_DYLIB \
  --sdk YOUR_IPHONEOS16_5_SDK
```

Apply the consolidated patch once; do not reapply the historical Mesa patches afterward. It reconstructs all 59 changed/new donor text source files. Native source builds were checked separately from the phone-tested artifacts; bit-identical reproduction of historical binaries is not claimed.

See the [feature build guide](source/screen-brightness/README.txt) for the separate Sileo system image and native brightness helpers. Generated firmware headers and binary dependencies remain local.

</details>

<details>
<summary><strong>Run the regression checks</strong></summary>

```sh
python3 tools/test-op13-dsi-brightness.py
python3 tools/test-op13-display-commit.py
python3 tools/test-op13-live-reset.py
python3 tools/test-op13-usb-reconnect.py
python3 tools/test-op13-turtle-overlay.py
python3 tools/test-op13-slider.py
python3 tools/test-op13-battery-wire.py
```

The native DSI, display and reset checks exercise implementation code under AddressSanitizer / UndefinedBehaviorSanitizer. The remaining checks cover reconnect handling, turtle/console memory guards, slider bus behavior and battery protocol boundaries. Tests requiring private firmware or historical diagnostic inputs remain separate.

Latest source checks: [29 October 2026](validation/20261029-source-checks.json).

</details>

## Credits

Built on **HackiOS / OOSEX GANG**, **Apple XNU**, **Project-Silicium**, **Mesa / Freedreno**, and **Qualcomm / OnePlus** platform work.

- GPU / GLES sources: [iola1999 kernel](https://github.com/iola1999/hackios-xnu-kernel/tree/k90-port) and [userland](https://github.com/iola1999/hackios-xnu-userland/tree/k90-port), recovered from the supplied OnePlus 13 GLES v3 archive.
- Battery transport reference: [ios-macan](https://github.com/hxfuxyy/ios-macan).
- Slider and Focus behavior: [haxi0's OnePlus 8 kernel](https://github.com/haxi0/iphont-xnu-kernel/tree/instantnoodle) and [userland](https://github.com/haxi0/iphont-xnu-userland), adapted to Dodge's AK09970 hardware.

Original notices and third-party licenses retain their scope. See [LICENSE](LICENSE) and [SOURCE-PROVENANCE.md](SOURCE-PROVENANCE.md) for attribution and integration boundaries.
