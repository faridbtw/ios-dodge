# OnePlus 13 Wi-Fi — verified 2026-10-30

Settings lists networks and joins the tested WPA2 network. Device logs show successful authentication,
association, WPA2/CCMP key installation, DHCP IPv4, DNS resolution, and an HTTPS
request to `https://example.com/` returning HTTP 200. The user confirmed a checkmark,
Wi-Fi off/on reconnection, and Safari loading the page.

## Selected RAM boot

From the project directory, with Android running and USB connected:

```sh
/opt/homebrew/bin/python3 tools/boot-op13-wifi.py --serial YOUR_SERIAL --output diagnostics/ios-wifi-next
```

Use a new output directory. Add `--menu` to upload and wait for the phone's
**Boot iOS** button. The automatic launch/bootstrap sequence was tested on the
phone; the Wi-Fi menu-wait variation has not been separately retested.

Offline package validation:

```sh
/opt/homebrew/bin/python3 tools/boot-op13-wifi.py --check
```

The helper reuses the verified firmware/rails/DMA/QMI preparation and exclusive
MHI ownership transfer. It uses `fastboot boot` and RAM staging; no flashing,
partition writes, or UEFI-variable writes. The previous GLES-only boot helper
does not prepare this Wi-Fi transport and remains a fallback for its old payload.
Network credentials are entered in iOS; this selected payload has no `fw/network`.
RAM-only network settings may need to be entered again after a cold boot.

| Artifact | Path | SHA-256 |
|---|---|---|
| Project Boot UEFI | `experimental/wifi/darwin-handoff-v25/project-boot-ios-uefi.img` | `2ee6c2a0def66dcb9ebe7a33268d3a1372202f1501de7219083fb647ab760955` |
| iOS payload | `experimental/wifi/darwin-station-v14/payload.bin` | `a876182c4edc815d706d76d7aa2a321b3913c52ba36e33d0aac1daf7d3d0f42f` |
| Kernel | Published source: `source/xnu-build-snapshot/xnu-8796.121.2` | `ca2cb1f8c057ceba910d0ae1b8f01f1270746380cfcf1931fa2c08f7de132147` |
| Matching local system | `experimental/sileo/v1/system-sileo.apfs` | `ece8098938ebbc265c8c782bf705cf2f74747deea002ec3a5548cb4307909ed3` |

## Changes that produced the working test

- Adapted the ios-macan LinuxKPI/ath12k station stack to the actual Peach
  PCIe identity `17cb:110e`, dynamic PCI window, owned SMMU/DMA pool, native
  firmware/QMI startup and adopted MHI/QRTR transport. Added explicit Peach
  hardware/profile handling rather than relabeling it as QCC2072.
- Matched native Peach CE settings: WMI buffers 3520 bytes, WMI RX 256 entries,
  HTT TX 256 entries, packet logging disabled. Audited descriptor/register layouts.
- Fixed QRTR four-byte packet padding and transferred transport ownership before
  starting XNU. Reserved separate DMA space for the imported driver.
- Restored Apple's stock `wifid` launch job, removed the unsupported device-tree
  load condition, and exposed the station through `en0`/Apple80211.
- Preserved successful scan results when a later scan fails. The user confirmed
  this changed the empty Settings list into a visible network list.
- Held the native **UMAC wake** request in addition to the existing MHI wake vote.
  Peach's native driver enables this mechanism, while the donor QCC2072 configuration
  disables it. Request: BAR0 + `0x3004 = 1`; acknowledgement: native register
  `0x01e04040 == 2`, accessed through a serialized temporary PCI window.
  The handshake must run **after WMI unified-ready**. An early test timed out;
  the corrected sequence was acknowledged and subsequent scan/join/data worked.
  Window selection is restored on all paths and failed requests release their vote.
- Used passive discovery during this bring-up. Association and WPA2 authentication
  remain real station operations. Fixed zero-initialized scan GET buffers omitting
  inline beacon information elements.
- Removed blocking optional statistics requests from the sole CE receive poller.
  Fixed copied build-cache dependency paths so all objects compile against the
  current source tree. Those changes alone did not resolve the connection stall.

The working kernel derives from the existing brightness/GLES build. Its boot
arguments retain seven iOS cores, firmware USB, Adreno rendering, live battery,
slider, flashlight, screen brightness and the existing wallpaper/display fixes.
Only `wifi=1` was added; the matching Sileo system image was unchanged.

## Source build

Build the driver with LLVM in PATH:

```sh
bash source/wifi-peach/darwin-ath12k-v2/build-ath12k.sh
```

Then build the active XNU snapshot with the driver object:

```sh
REDWOOD_OBJECTS="$PWD/source/wifi-peach/darwin-ath12k-v2/out/ath12k.o" \
  python3 tools/build-xnu-macos.py build \
  --source source/xnu-build-snapshot/xnu-8796.121.2 \
  --work build/mac-xnu-peach-wifi-v2 --deps build/mac-xnu/deps -j6
```

Run the harness's export/libs stages first for a fresh work directory. Restore
local SDK/build dependencies as described in SOURCE-PROVENANCE.md. The selected
phone artifact also contains paused, gated audio diagnostics omitted from this
publication; source integration is preserved, but binary identity is not claimed.
`experimental/wifi/darwin-peach-v2/prepare-station-payload.py` repacks a matching
local baseline with the new kernel and restored stock wifid job. Local source
images, Apple stock job and BUILD metadata must be supplied; no credentials are
bundled in the selected profile. Boot helpers require the matching generated
payload/system/UEFI images and their local manifests.

## Validation and scope

- Evidence: `diagnostics/20261030-wifi-macan/darwin25-umac-ready/RESULT.json`,
  `ios-user-joined.log`, `ios-reconnected.log` and runtime captures.
- Actual UMAC helper: ASan/UBSan tests cover acknowledgement, bounded timeout,
  exact offsets, PCI window preservation and failure rollback.
- Actual scan-result encoder: ASan/UBSan checks cover zero/full/limited IE
  capacity, SSID/rates and bounded copyout. Driver/kernel builds and private
  APFS file readback verification passed; the normal helper's offline check passed.
- Roaming, long idle/suspend, sustained throughput, other security modes, and
  cold-boot credential retention need further testing. Sileo package downloads
  were not separately tested. Reconnection/Safari are user-confirmed; no claim
  is made that every reconnect mechanism has been exhaustively traced.

Source and native-driver revisions/licenses are recorded under
`source/wifi-peach/darwin-ath12k-v2/UPSTREAM.json` and its audit files. System
images, private firmware/calibration, credentials and raw device logs stay local.
