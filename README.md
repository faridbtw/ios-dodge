<p align="center">
  <img src="assets/readme/banner.svg" width="1120" alt="ios-dodge — OnePlus 13 iOS port">
</p>

<p align="center">
  <strong>OnePlus 13</strong> &nbsp; / &nbsp; <strong>iOS 19.0 · 23A191a</strong>
</p>

<p align="center"><a href="QUICKSTART.txt">Quick start</a> · <a href="source/ios19/README.md">Source & build notes</a> · <a href="BUGLIST.txt">Known issues</a> · <a href="validation/ios19/STATUS.json">Validation</a></p>

---

The **iOS19** branch preserves the Solarium bring-up for OnePlus 13 (`dodge`, SM8750). It includes the source changes behind the tested Adreno/Metal UI, native EDR responsiveness fix, wallpapers, battery/Island layout and alert slider.

**RAM only:** use `fastboot boot` and temporary staging. The repository remains source-only: system images, SDKs, firmware, device calibration and raw logs stay local. The iOS16 and iOS27 branches remain separate.

[Build locally from source](docs/BUILDING.md) — requirements, per-version recipes and the remaining local packaging steps. UEFI, payloads, system images, firmware and calibration are not distributed.

## Current status

| Feature | iOS 19 result |
| :--- | :--- |
| Wake and touch | User-confirmed working. |
| Adreno 830 / Metal | Native UI renders; much smoother scrolling confirmed after the EDR fix. Two five-second windows reached 52 and 59 presented FPS. |
| Wallpapers | Display and wallpaper selection user-confirmed working. |
| Battery | Native live DSP/IOKit/IOPS data and visible indicator work. Raw SOC accuracy/smoothing still needs checking. |
| Dynamic Island / battery placement | User-confirmed; guarded native layout, battery inset 22 points. |
| Alert slider | Bottom **Ring**, middle **Silent**, top **Do Not Disturb**. All positions user-confirmed; native Focus take/release succeeded. |
| CPU / USB | Seven iOS cores; one firmware core retains diagnostic USB access. |

## Still open

The status-bar clock remains absent. DSP raw SOC was 100% versus an earlier Android UI reading of 90%; percentage accuracy and this profile's unplug/reconnect charging transitions need further validation. Icon-color customization can respring. Wi-Fi and audio integration are disabled; ordinary app availability is limited by the internal source image. Neither universal 60/120 FPS nor broad app compatibility is established.

## Repository layout

| Folder | What you’ll find |
| :--- | :--- |
| [Source](source/) | Kernel, UEFI, graphics and native platform adaptations. |
| [Tools](tools/) | Build, packaging, boot and regression helpers. |
| [Validation](validation/) | Results scoped to each tested build. |
| [Documentation](docs/) | Guides, source credits and archived instructions. |
| [Assets](assets/) | Banner and boot artwork. |
| [Hardware references](diagnostics/) | Vendor protocol source references. |
| [Integration helpers](experimental/) | Source scripts for staged bring-up. |

## Sources and booting

Start with [source/ios19](source/ios19/README.md). It contains kernel and sampler patches against the preserved iOS27 bases, the exact tested native compatibility sources, a helper build script and a profile-driven RAM boot runner. [QUICKSTART.txt](QUICKSTART.txt) distinguishes source-checkout commands from the owner's prepared local iOS16/27/19 workspaces.

```sh
python3 source/ios19/tools/build-compat.py --output build/ios19-root --bootstrap
python3 source/ios19/tools/boot-ios19.py --profile /path/to/BOOT-PREFLIGHT.json --serial YOUR_SERIAL --check
python3 source/ios19/tools/boot-ios19.py --profile /path/to/BOOT-PREFLIGHT.json --serial YOUR_SERIAL
```

A compatible local SDK, prepared images, firmware and dependencies are required; this is not a downloadable OS bundle or a one-command image builder. Keep USB connected through upload. After startup, unplugging leaves iOS running from RAM until the next reboot.

Earlier source snapshots and their licenses remain in `source/`. See [sources and credits](SOURCE-PROVENANCE.md) and the [iOS19 change/build details](source/ios19/README.md).
