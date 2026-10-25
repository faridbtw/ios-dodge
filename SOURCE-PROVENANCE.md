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
- GPU command submission and 120 FPS remain unverified/unfinished.

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
