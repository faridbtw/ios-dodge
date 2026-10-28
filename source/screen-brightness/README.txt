Confirmed feature integration, 29 October 2026

Current local profile: screen-brightness/control-v2 with screen-brightness-v13
UEFI and the Sileo system image, as identified in root QUICKSTART.txt.
No OS images, SDK, firmware, downloaded packages or raw logs are distributed.

Screen brightness
-----------------
Settings and Control Center visibly dim/brighten the panel (user confirmed).
brightness.c reads float BKSDisplayBrightnessGetCurrent(void), verified in
iOS16.5/20F66, approximately every100ms. Only changed levels are written to
root-only /dev/op13-screen. The native kernel worker sleeps between updates.

op13_screen.c and op13_dsi_brightness.h in the active XNU tree implement the
bounded DSI0 FIFO DCS51 packet. Normal maxDBV3515, minimum8, no HBM or panel-off
requests. It restores command controller settings and preserves unrelated
pending interrupts. A failed transfer stops writes; a timeout requires reset.
There is no framebuffer dimming or renderer change.

The firmware must vote disp_cc_mdss_ahb_clk before XNU starts. The standard
boot helper sends "oem op13-dsi-brightness ahb" and verifies controller version
and clock state. This is a configuration clock vote, not a pixel/byte/PHY
frequency, mode or refresh change. The protocol-only brightness probe found
no installed Qualcomm DisplayPwrCtrl server, so it is not the active path.
All new diagnostics remain blocked while iOS owns the touch/display lease.

cc-brightness.c wraps the existing SpringBoard injection library: it first
loads original libredwood-sb.dylib unchanged. A main-queue timer waits for
DisplayModule, verifies its exact method address plus shared-cache slide, and
overrides only CCUIDisplayModuleViewController's _canAdjustBrightness while
/dev/op13-screen exists. This enables its otherwise stuck slider. It does not
change global BKS capability reports, ALS arrival, touch setup or flashlight.
Automatic ambient-light brightness remains unverified.

Flashlight and Sileo
--------------------
The active redwood_torch.c uses PM8550 SID1/EE0, four channels, requested32mA
each, effective30mA each. User confirmed Control Center on/off and levels.
The sleeping worker retains fault/temperature checks and a30-second cutoff;
turn off/on for another activation. It shares the existing SPMI transaction
lock with buttons. GPIO, LED current and panel brightness are separate paths.

Sileo2.5.1 opens (user confirmed), with Procursus1800, ElleKit0.4.5 and
PreferenceLoader2.2.6-1. The prefix is grafted into RAM each boot; installed
packages/settings do not persist across a cold reboot. Network downloads and
arbitrary tweak compatibility remain unverified; Wi-Fi is unfinished.
The supplied mk-jb.sh retains upstream behavior; the command below selects
the smaller package set used in the tested build. No SSH service is started.

Build with local inputs
-----------------------
1. Restore the SDK, XNU dependency tree and original local boot/payload inputs
   described in SOURCE-PROVENANCE.md. Use a unique XNU work directory if
   switching source checkouts: generated dependency files keep absolute paths.
   Build the active source with tools/build-xnu-macos.py export, libs, build.

2. Build our two small userland helpers:

python3 tools/build-op13-brightness-userland.py --sdk YOUR_IPHONEOS16_5_SDK

3. Supply a prepared rootless bootstrap, or generate it from the retained
   upstream script (requires its normal host dependencies and network):

PREINSTALL="debianutils launchctl preferenceloader" bash source/hackios-xnu-userland/tools/mk-jb.sh

4. Repack your original matching system image into a separate local Sileo
   image. The builder checks the original image hash and reads every original
   regular file back after repacking. It never modifies the original image.

python3 tools/make-op13-sileo-system.py --system YOUR_ORIGINAL_SYSTEM_APFS --bootstrap source/hackios-xnu-userland/jb/root/var/jb --output build/sileo-system

5. Prepare a new payload from your local working GLES/wallpaper payload and
   its adjacent BUILD.json. It preserves the existing injected libraries,
   adds Sileo startup jobs and screen helpers, checks the native screen/torch
   kernel markers, and verifies every regular RAM-root file after repacking:

python3 tools/make-op13-feature-payload.py --base-payload YOUR_BASE_PAYLOAD --kernel YOUR_NEW_KERNEL --system build/sileo-system/system-sileo.apfs --brightness-userland build/screen-brightness --sileo --output build/features

Use the corresponding local v13 UEFI image/packaging manifests at the path
expected by QUICKSTART.txt. The source-only build is not claimed byte-identical
to the historical phone artifacts; the boot helper pins tested firmware.
Read-only package check, then follow QUICKSTART with a connected Android phone:

python3 tools/boot-op13-gles-v3.py --payload build/features/payload.bin --system build/sileo-system/system-sileo.apfs --check

RAM ONLY: fastboot boot plus temporary staging; never flash or erase.

Validation and remaining display issue
-------------------------------------
python3 tools/test-op13-dsi-brightness.py exercises the actual packet helper
under ASAN/UBSAN, including bounds, W1C preservation, saved-state restoration,
busy/error and timeout rejection. Kernel and firmware copies must match.
Other established display, reset, USB, turtle, slider and battery tests remain.

validation/screen-brightness, flashlight, sileo and buttons contain sanitized
results and hashes. Physical panel dimming, both brightness UIs and flashlight
levels are user-confirmed. The saved brightness build also restored Power and
both volume buttons after an experimental display build had lost them.

Occasional app-dismiss tearing is minor and deprioritized by the user. The
nonpreemptible-copy experiment is not selected or included in this source
update; its button failure cause is unproven. Existing shadow/TE presentation,
normal brightness and working buttons take precedence. Sound and FPS work
remain paused.
