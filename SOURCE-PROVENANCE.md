# Source and validation provenance

This repository is a source snapshot of the working OnePlus 13 development bundle, prepared on 25 October 2026. It does not claim that one upstream commit reproduces the final image without the port changes and omitted local inputs.

## Bases and references

- HackiOS kernel: `oosexgang/hackios-xnu-kernel`, base `8e61187098f2c26c5daee8f9f72b6416aa64b9ab`.
- HackiOS userland: `oosexgang/hackios-xnu-userland`, base `906425b75e02b4f99eb5d7bb1ef7c9583472d772`.
- Apple XNU: `xnu-8796.121.2`, base `c54f35ca767986246321eb901baf8f5ff7923f6a`.
- Modern ADSP reference: `hxfuxyy/ios-macan`, `bf6106be492670acd05fac0ee7b7d037a48eafd9`. Its memory addresses and direct gauge wiring must not be copied unchanged to Dodge.
- OnePlus SM8750 PAS reference: `OnePlusOSS/android_kernel_oneplus_sm8750`, `6028f47faddaa27700f8dd3a1d83906ea8f27170`.
- Vendor PMIC ABI: the captured `oplus_hal_adsp.[ch]` under the pinned OnePlus SM8750 modules checkout; see the accompanying source notices. GET property IDs and units were checked against these files.
- Temporary boot image: Project-Silicium Mu-Silicium Dodge v3.1 based RAM loader route.

The XNU snapshot includes both active source and historical `files/` overlays. The active build reads the ordinary source paths. Do not assume the historical overlays contain the latest battery changes.

## Validated behavior

- The user reported the original CoreAnimation software compositor is faster than Mesa softpipe.
- The battery diagnostic collected 39 fresh samples with changing current, voltage and temperature, charging status, and successful timed cold-reset recovery.
- The user confirmed percentage and charging indication in the eight-core interactive build, then confirmed the charging bolt follows unplug/reconnect.
- This describes the earlier software-renderer baseline. The 28 October GPU/display validation below supersedes its GPU status;120FPS remains unverified.

Selected build and status metadata is preserved under `validation/`. Raw device logs, identifiers, firmware dumps and Apple images are omitted.

## Local artifact identities

The hashes in `validation/live-original-compositor-v1/BUILD.json` identify the combined live battery build. Those binaries are local inputs, not uploaded artifacts. The diagnostic RAM loader used for that build has SHA-256:

`29f2d04a2db4a684d6436564e3f3a916fbde01bb7081a9604882665b4052a86e`

The faster compositor-only image used the earlier validated kernel with SHA-256:

`f81b042837eff9943dda34ef462f15607f4c3e3d5fe9ea581e64ab6aa61fc92c`

## License boundaries

Retain the original notices in every source subtree. The kernel project and XNU include Apple Public Source License material; userland has its upstream MIT license; the two vendor PMIC reference files carry GPL-2.0-only notices. Firmware byte arrays without a clear source license, vendor UEFI binary resources, SDKs, firmware images and extracted iOS system components are excluded from this snapshot.

## Alert slider

- OnePlus 8 sensor/event reference: `haxi0/iphont-xnu-kernel`, branch `instantnoodle`, commit `de24a3774550ef74e28301e6174f29f08fba4360`.
- iOS ringer/Focus reference: `haxi0/iphont-xnu-userland`, commit `d2c1e870d39ff79a08988e58fd15787200d0faf5`.
- Dodge AK09970 register protocol: OnePlus SM8750 vendor modules commit `a85bac41e21a790e216039cde1d34a6c5d6416d1`, `vendor/oplus/kernel/device_info/tri_state_key/digital_hall_ic/oplus_ak09970.c`.
- Hardware profile: captured phone FDT identifies AK09970 at I2C0x10 on GPIO80/81, supplied by pm_humu_l1/RPMh ldob1 at1800mV. The older OnePlus8 IST8801 pair was not copied as a hardware profile.
- Firmware RPMh/CmdDb ABI was checked against the actual phone's UEFI binaries. Extracted binaries and vendor SDK headers are excluded.
- The timed native test captured bottom/middle/top events and SpringBoard switching to silent. The user confirmed Silent Mode and Do Not Disturb in the final eight-core build. The final DND service requests execute on a background queue.
- `validation/alert-slider/` records artifact hashes and sanitized results. Factory calibration is supplied locally and is redacted from shared command-line metadata. Device logs and system/firmware images are excluded.

## UEFI display and USB reconnect

- Current loader source is published at `source/touch-runtime-v15/`, matching the locally phone-tested `stable-overlay-v11` profile. The baseline firmware route and omitted binary inputs remain unchanged.
- The Qualcomm USB Device Protocol ABI was checked against `Silicon/Qualcomm/QcomPkg/Include/Protocol/EFIUsbDevice.h` in Project-Silicium/Mu-Silicium at `4cba43c90e02cdbefbcd9fcdbda33d2328deaec0`. No vendor firmware binaries or SDK headers are added by this update.
- The larger console uses the existing XNU ISO Latin-1 font, retaining its source notices and the font author's permission notice in `op13_console_font.h`.
- Turtle artwork was generated for this project. `assets/uefi/` includes the original PNG, native bitmap and prompt; `turtle_pixels.h` contains the derived grayscale pixels used by the loader.
- The stable overlay uses owned cached RAM for the text surface. Its sanitizer test compares underlying text against a normal full-screen console and checks that the physical turtle remains visible throughout updates, scrolling and memory guard checks.
- Two physical cable cycles passed on the `usb-reconnect-v8` profile. Commands recovered in the same session; a previously staged payload remained usable, and new uploads/log downloads passed. The USB transport and polling functions are unchanged in `stable-overlay-v11`.
- Repeated scrolling on the final profile remained responsive. The same loader then started the working eight-core iOS payload with battery and slider support.
- `validation/uefi/` contains sanitized status and artifact hashes. Boot images, system images, raw device logs, session cookies and device identifiers are omitted.

## 28 October 2026 GPU, display, wallpaper and live reset update

- Source input: user-supplied `OnePlus13-GLES-v3-Sources-20261028-191401.tar.7z`, recovered v3-dt-concurrency. Its18,825 regular-file hashes and10 tar symlinks were checked locally.
- Archive runtime source pins: kernel `dc49065f8f17c3683925270388eba128f28eab33`, userland `2051390a14a9d967d185bd8f294fc086bf29c8e8`, workspace `10e32a564c19ac10d4cf86a085b9d3803e030fab`. The recovered v3 also includes later documented concurrency repairs; these pins identify provenance, not a complete reproducible upstream checkout.
- Paired Adreno830 service/BO/context/HWSCHED/SMMU code, GLES bridge and Mesa source retain their existing notices. `source/mesa26-op13/` records the exact upstream26.2.5 tar hash and consolidated59-file patch. Applying it to the verified tarball reproduced every recorded changed-file hash; generated Python caches/rejections were excluded.
- The active XNU tree integrates the GPU code with existing battery and slider support. Secure device-tree searches use per-call traversal state; immutable board matching is cached before concurrent GPU clients start. Unsafe reuse of GPU-owned memory remains blocked.
- `MiniFramebufferCommit.h` and the active framebuffer driver add full-frame shadow composition and bounded read-only GPIO86 TE synchronization. No display-register, pinmux or panel-mode writes are introduced.1,187 sanitizer frame-copy/timing cases passed. User confirmed horizontal tearing is gone.
- `source/wallpaper-preview/preview.m` replaces only the verified20F66 `PBFHomeScreenPreviewImageBuilder _imageForView:` implementation, after checking its cache-relative address and ObjC ABI. Native catalog services are enabled on demand. The lock-screen picker remains guarded, not fixed.
- `source/touch-runtime-v15/loader.c` now matches the v12 live-reset behavior. Exact Dodge reboot commands reach EFI cold reset before touch guards.192 sanitizer dispatch cases and an actual live-iOS-to-Android reset passed.
- The completed live-USB profile is included. Unfinished sound/Wi-Fi experiments and audio diagnostic code from the local workspace are not included in this publication. The battery source retains the tested native transport and adds only the matched seven-core GLES profile acceptance.
- The checkout's native kernel is built separately with restored local-only dependencies, including the ignored legacy FocalTech firmware header. Phone validation summaries identify the original local test artifacts; this source-only publication does not claim their binaries are byte-identical to a rebuild with paused diagnostics excluded.
- `validation/gpu-gles/`, `validation/display/`, `validation/wallpaper/`, and `validation/uefi/live-reset/` hold sanitized results and hashes. Factory slider calibration is redacted. Firmware, Apple files, device identifiers, private network data and raw logs remain local.

## 29 October 2026 Sileo, flashlight and screen brightness update

- Native torch is adapted to PM8550 SID1/EE0 and all four Android torch channels. Requested32mA maps to effective30mA/channel. The user confirmed on/off and levels. Fault/thermal checks, a sleeping worker and30-second cutoff are retained.
- AA569 panel identification, normal DBV3515, inverted/high-byte-first DCS51 and DSI0 controller addresses come from the captured OnePlus device tree. The FIFO sequence and packet layout were checked against native Qualcomm `dsi_ctrl.c`, `dsi_ctrl_hw_cmn.c` and `dsi_ctrl_reg.h` in the pinned SM8750 modules source. The new helper is independent code with a BSD-2-Clause notice.
- ClockDxe revision1000b uses only its verified configuration-AHB enable API. The firmware DisplayPwrCtrl SDK protocol was inspected but is absent at runtime. No extracted vendor headers or binary modules are published.
- Userland ABI was checked against locally supplied iOS16.5/20F66 frameworks. `BKSDisplayBrightnessGetCurrent` is float(void); the Control Center override checks its exact method address and cache slide. No Apple binaries, extracted disassembly or caches are included.
- Sileo2.5.1, Procursus1800 rootless, ElleKit0.4.5 and PreferenceLoader2.2.6-1 were prepared through the retained upstream `mk-jb.sh`; its line endings are normalized for the host shell. Downloaded packages/bootstrap stay ignored. The new system repack helper preserves every original regular file and creates a separate local image. No SSH daemon is started.
- The GLES bridge carries the narrow untextured GL_LINES glyph-spans exception, retaining the rejection of untextured full-screen triangles.
- Native source builds and helpers are validated separately from historical phone binaries. The handset kernel also contains gated, unfinished audio diagnostics that remain excluded from this source update. They are not claimed as a sound fix; bit-identical reproduction of the phone kernel is not asserted.
- The nonpreemptible-copy display experiment is excluded: the user reported button failure while it ran; restoring the selected brightness build recovered Power and both volume buttons. The cause is unproven, and remaining minor app-dismiss tearing is deprioritized.
- `validation/screen-brightness`, `flashlight`, `sileo`, `buttons` and `uefi/BRIGHTNESS-v13.json` contain sanitized results. Runtime images, raw logs, calibration, credentials, firmware and SDKs remain local.
