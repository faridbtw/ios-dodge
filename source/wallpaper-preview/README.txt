OnePlus13 GLES wallpaper preview helper
======================================
BSD-2-Clause source: preview.m.

The iOS16.5/20F66 PBFHomeScreenPreviewImageBuilder _imageForView: method creates
a Metal texture and places it in a dictionary. Metal is disabled on the GLES
port, so a nil texture makes PosterBoard abort. This helper is injected only
into PosterBoard and replaces that exact method with a CoreGraphics CALayer
capture. It verifies the expected shared-cache-relative IMP and ObjC signature
before changing it. Main GPU rendering, Apple binaries, and system.apfs stay
unchanged. No generic nil dictionary or global exception suppression is used.

Compile from the project root:
/usr/bin/clang -target arm64-apple-ios16.5 -isysroot build/mac-xnu/deps/ios-sdks/iPhoneOS16.5.sdk -O1 -Wall -Wextra -fblocks -dynamiclib -install_name /redwood/lib/libredwood-wallpaper.dylib -framework UIKit -framework QuartzCore -framework CoreGraphics -framework Foundation -lobjc -o experimental/wallpaper/libredwood-wallpaper-guarded.dylib source/wallpaper-preview/preview.m

Package into a NEW output directory:
/opt/homebrew/bin/python3 tools/make-op13-wallpaper-payload.py --output experimental/wallpaper/NEW --preview-library experimental/wallpaper/libredwood-wallpaper-guarded.dylib

Default paired loader is phone-tested v12 if its validation receipt is present.
Live validation found the exact method, generated four preview images, and
showed no PosterBoard SIGABRT in the first60s capture. User confirmed Settings
selection works. Long-term stability remains unverified. Lock-screen long-press
selection is deliberately still guarded because its hosted scene renders black.
Do not describe this as a complete fix for that separate lock-screen picker.

Raw captures, Apple assets/firmware, system.apfs and boot roots stay private.
Sanitized test results: validation/wallpaper/STATUS.json and QUICKSTART.txt.
