# Building locally

Build the port's source components locally and keep Apple images, SDKs, extracted firmware, slider calibration and generated boot files off GitHub. This guide does not grant rights to third-party software; retain the licenses in each source tree and obtain permission for the inputs you use. See [source provenance](../SOURCE-PROVENANCE.md).

This repository is a development source snapshot. It does **not** contain a complete, independently reproduced, one-command builder for every tested phone image. The working profiles also depend on local inputs and packaging steps. Commands below distinguish source compilation from assembling a bootable phone image.

## 1. Select the matching branch

Use a separate checkout or Git worktree for each version. From a checkout with access to the private repository:

```sh
git fetch origin
git worktree add ../ios-dodge-build16 origin/iOS16
git worktree add ../ios-dodge-build19 origin/iOS19
git worktree add ../ios-dodge-build27 origin/iOS27
```

| Branch | Runtime | Source/build notes |
| :--- | :--- | :--- |
| `iOS16` | iOS 16.5 / 20F66 | [Feature integration](../source/screen-brightness/README.txt), [Mesa](../source/mesa26-op13/README.txt), [Wi-Fi](../source/wifi-peach/WORKING.md) |
| `iOS19` | Solarium iOS 19.0 Internal / 23A191a | `source/ios19/README.md` and `source/ios19/UPSTREAM.json` on that branch |
| `iOS27` | iOS 27.2 / 24B119 | `source/ios27/README.md` and `source/ios27/UPSTREAM.json` on that branch |

Later source directories are available only on their corresponding branches and descendants. Keep each version's kernel, renderer, ramdisk, UEFI and system matched. The internal iOS19 source archive is a user-supplied local input; this guide does not provide a download.

## 2. Prepare host tools and local inputs

The existing build helpers target macOS. Install Xcode/command-line tools, Python 3.11+, LLVM with `lld-link`, and Android platform tools. The renderer additionally needs the Rust/C++ tools described in its pinned source notes.

Check the tools before building:

```sh
python3 --version
xcrun --find clang
command -v lld-link
command -v adb
command -v fastboot
```

With Homebrew LLVM on an Apple Silicon Mac, make its compiler available to the loader build:

```sh
export PATH="$(brew --prefix llvm)/bin:$PATH"
```

Keep dependencies and generated outputs under the ignored `build/` directory or outside the checkout. The XNU helpers expect pinned iPhoneOS16.5 and MacOSX13.3 SDK layouts plus Apple open-source dependencies, even for the newer port kernels. Read the selected builder and its dependency `setup.sh` for the exact directory names/revisions. The iOS19 helper compilation separately uses the SDK selected by `xcrun --sdk iphoneos`.

Prepare locally:

- A compatible SDK and the pinned open-source dependencies.
- The exact version's prepared system/root and bootstrap inputs; a random stock APFS image is not interchangeable.
- Matching OnePlus 13 firmware from an authorized source, including any local header or link object needed by the selected kernel.
- Your own handset's AK09970 calibration if enabling the alert slider. Its format is nine signed integers: top XYZ, middle XYZ, bottom XYZ. Do not substitute another handset's values.

No firmware/calibration values or SDK downloads are supplied here. Wi-Fi source compilation does not create the proprietary Wi-Fi firmware.

## 3. Compile the RAM-only UEFI loader application

For iOS16, use `source/touch-runtime-v15`. On iOS19/27, use the version's loader source at `source/ios27/xnuloader`. These are UEFI application sources, separate from the phone's complete UEFI firmware image.

Run from the repository root, choosing one source path:

```sh
# iOS16:
LOADER_SRC=source/touch-runtime-v15
# For iOS19/27 instead: LOADER_SRC=source/ios27/xnuloader

mkdir -p build/loader-source
git archive HEAD "$LOADER_SRC" | tar -x -C build/loader-source
RAM_ONLY=1 LOADER_PLATFORM=dodge EMBEDDED_USB=1 \
  bash "build/loader-source/$LOADER_SRC/build.sh"
```

Output: `build/loader-source/$LOADER_SRC/loader.efi`. The isolated archive copies tracked source only, leaving working source and historical local artifacts untouched. Use a fresh build directory when changing branches/revisions.

Creating `dodge-ios-uefi.img` also requires the correct Dodge Mu-Silicium base, board device tree, firmware modules, BootShim and matching embedded-loader packaging. These inputs stay local. The repository does not automate a complete clean Dodge image rebuild from independently supplied inputs. The retained `source/hackios-xnu-kernel/uefi/README.md` describes a different board; use it as architectural background, not as OnePlus 13 hardware instructions. `tools/make-op13-turtle-uefi.py` customizes an existing compatible local image; it is not a full UEFI firmware builder.

The shared loader application was compiled from an isolated tracked-source copy on 5 November 2026. That validates compilation, not full UEFI image reconstruction or a new phone boot.

## 4. Build the kernel and graphics components

### iOS16

First arrange the dependency tree described by `source/hackios-xnu-kernel/xnu-deps/setup.sh`. Use the active XNU snapshot, a fresh work directory and explicit paths:

```sh
python3 tools/build-xnu-macos.py --help

# Replace /path/to/local/xnu-deps with your prepared dependencies.
for action in export libs build; do
  python3 tools/build-xnu-macos.py "$action" \
    --source "$PWD/source/xnu-build-snapshot/xnu-8796.121.2" \
    --work "$PWD/build/ios16-xnu" \
    --deps /path/to/local/xnu-deps -j 4 || break
done
```

The exact GPU and Wi-Fi integrations need their corresponding link inputs; a plain kernel compilation does not establish the current accelerated Wi-Fi profile. Follow `source/mesa26-op13/README.txt`, `source/wifi-peach/WORKING.md` and the source provenance records. Older reference-only ath12k recipes and profiles are not substitutes for the validated Peach/MHI adaptation.

For the userland brightness helpers, with a local SDK:

```sh
python3 tools/build-op13-brightness-userland.py \
  --sdk /path/to/iPhoneOS16.5.sdk --output build/screen-brightness
```

`source/screen-brightness/README.txt` describes the local Sileo/bootstrap and feature-payload recipes. They require a matching pre-existing local GLES/wallpaper root/payload and other omitted inputs; they do not create those inputs from a fresh checkout.

### iOS27

Start with `source/ios27/README.md` and its pinned dependencies. The kernel base is `source/ios27/xnu-xnu-12377.121.6`; the native frontend, translator and PM4 sources are under `source/ios27/gpu/`.

```sh
python3 source/ios27/tools/build-kernel-macos.py --help
```

The builder supports `export`, `libs` and `build`, explicit `--source`, `--work`, `--deps`, `--pthread-source` and `--wifi-object`, and defaults to `arm64e`. Supply the pinned newer pthread tree and the required ath12k link object even for the selected Wi-Fi-disabled profile. Its default paths describe the original owner's workspace, so pass explicit paths for another machine. Restore omitted local firmware inputs before compiling the affected source.

### iOS19

Use `source/ios19/README.md`. Copy the included iOS27 kernel and translator bases into separate ignored work directories, then check and apply the iOS19 patches:

```sh
mkdir -p build/ios19-bases
git archive HEAD source/ios27/xnu-xnu-12377.121.6 source/ios27/gpu/metal2vulkan \
  | tar -x -C build/ios19-bases

REPO_ROOT="$PWD"
(
  cd build/ios19-bases/source/ios27/xnu-xnu-12377.121.6
  git init -q
  git apply --check "$REPO_ROOT/source/ios19/patches/kernel-ios19.patch" &&
  git apply "$REPO_ROOT/source/ios19/patches/kernel-ios19.patch"
)
(
  cd build/ios19-bases/source/ios27/gpu/metal2vulkan
  git init -q
  git apply --check "$REPO_ROOT/source/ios19/patches/metal2vulkan-legacy-sampler.patch" &&
  git apply "$REPO_ROOT/source/ios19/patches/metal2vulkan-legacy-sampler.patch"
)
```

Build the patched kernel using the iOS27 harness with explicit source/work/dependency options. Build the translator/frontend using the matching version's renderer recipes and pinned tools. The compatibility helpers have a portable builder:

```sh
python3 source/ios19/tools/build-compat.py --output build/ios19-root --bootstrap
```

This builds/ad-hoc signs the project helpers locally; it does not create the Apple runtime, complete root filesystem, system image or payload. `source/ios19/tools/workspace/` contains the original workspace recipes; those require a prepared `IOS19_RAM_DIR` and the original local inputs described by the branch guide.

## 5. Assemble matching local images

A phone boot needs the complete version-matched set:

| Local artifact | How it is prepared |
| :--- | :--- |
| `dodge-ios-uefi.img` | Locally package the Dodge firmware base and matching RAM loader. |
| `payload.bin` | Package the compiled kernel, prepared APFS bootstrap ramdisk and complete matching command line. |
| `system.apfs` | Prepare the corresponding runtime/system locally, retaining the version-specific compatibility work. |
| Wi-Fi bundle, iOS16 only | Prepare the firmware expected by the exact Peach/MHI host setup. |
| Calibration, when slider enabled | Obtain your own handset's calibration and supply it locally to the matching payload. |

`source/hackios-xnu-kernel/xnuloader/mkpayload.py` documents the XNUP container format. It packages supplied files; it does not build the kernel or prepare an iOS runtime. The bootstrap ramdisk is embedded in `payload.bin`, so uploading does not require a separate copy of `boot.apfs`.

Preserve RAM-only arguments such as `ufs=0 ufs_write=0 var_store=0` and the complete selected profile's other settings. Do not create a working command line from this abbreviated list. Renderer, touch, USB, battery, CPU and Wi-Fi ownership settings must agree across firmware and kernel. Record file sizes and SHA-256 hashes in a private local profile after rebuilding; historical hashes identify the original local builds and will not necessarily match a rebuild.

## 6. Validate before a RAM boot

Use the matching branch's `QUICKSTART.txt` and host helper. Original workspace helpers may pin historical hashes or assume prepared directories. They need a matching regenerated local profile/packaging manifest before accepting a new build; disabling integrity checks is not a substitute.

For the portable iOS19 runner, with your own prepared local profile:

```sh
python3 source/ios19/tools/boot-ios19.py \
  --profile /path/to/BOOT-PREFLIGHT.json --serial YOUR_SERIAL --check
```

Only after validation, start Android, enable USB debugging, authorize the computer and use the corresponding boot helper. The phone firmware must be loaded with **`fastboot boot` only**. Upload the matching payload/system into RAM. The iOS16 Wi-Fi profile additionally requires its ordered rails/DMA/QMI and exclusive MHI handoff before iOS starts. A bare UEFI boot does not perform all that preparation.

Keep USB connected through uploading and startup. A cold restart discards the RAM session. Start a new session after rebooting Android rather than reusing stopped GPU/DSP resources. Building a component successfully does not establish that the complete new image is safe or functional on hardware.

## 7. Keep private/generated inputs out of commits

Generated `.img`, `.efi`, `.bin`, `.apfs`, firmware, SDKs, calibration and raw logs stay local. `.gitignore` is a convenience rather than a secrets scanner. Stage only the intended source/documentation changes and review `git diff --cached --name-status` before pushing. Preserve third-party source licenses and notices. Rebuilding locally does not itself establish permission to redistribute its inputs or output.
