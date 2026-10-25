#!/usr/bin/env python3
"""Build the bundled Mesa/EAGL sources for iOS on macOS, without Linux wrappers.

Run with a Python environment containing meson, ninja, mako, pyyaml and packaging.
This builds software rendering only; it does not implement Adreno 830 support.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
USER = ROOT / 'source/hackios-xnu-userland'
VERSION = '25.0.7'
SHA = '592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6'


def run(*args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work-dir', type=Path, default=ROOT / 'build/macos-mesa')
    parser.add_argument('--output', type=Path, default=ROOT / 'experimental/mesa-softpipe')
    args = parser.parse_args()
    work, out = args.work_dir.resolve(), args.output.resolve()
    work.mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)
    # Use meson/ninja installed beside the selected Python, including a private venv.
    os.environ['PATH'] = str(Path(sys.executable).parent) + os.pathsep + os.environ['PATH']
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    archive = work / f'mesa-{VERSION}.tar.xz'
    if not archive.exists():
        run('curl', '--fail', '--location', '--output', archive,
            f'https://archive.mesa3d.org/mesa-{VERSION}.tar.xz')
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA:
        raise SystemExit('Mesa archive SHA-256 mismatch')
    patches = sorted((USER / 'tools/mesa').glob('*.patch'))
    digest = hashlib.sha256(''.join(p.read_text() for p in patches).encode()).hexdigest()
    src = work / f'mesa-{VERSION}'
    stamp = src / '.op13-patches'
    if not stamp.exists() or stamp.read_text().strip() != digest:
        if src.exists():
            raise SystemExit(f'{src} already exists without matching patch provenance; use a fresh --work-dir')
        with tarfile.open(archive) as tar:
            # Only the verified, pinned upstream archive is extracted.
            tar.extractall(work)
        for patch in patches:
            # read_text translates the CRLF files in the Windows export to LF.
            run('patch', '-s', '--no-backup-if-mismatch', '-d', src, '-p1',
                input=patch.read_text(), text=True)
        stamp.write_text(digest + '\n')
    for name, target in [('redwood-drm', 'src/redwood-drm'), ('air', 'src/compiler/air')]:
        shutil.copytree(USER / 'tools/mesa' / name, src / target, dirs_exist_ok=True)
    cross = work / 'ios-cross.ini'
    cross.write_text(f'''[binaries]
c = ['clang', '-target', 'arm64-apple-ios16.5', '-isysroot', '{sdk}']
cpp = ['clang++', '-target', 'arm64-apple-ios16.5', '-isysroot', '{sdk}']
objc = ['clang', '-target', 'arm64-apple-ios16.5', '-isysroot', '{sdk}']
ar = 'ar'
ranlib = 'ranlib'
strip = 'strip'
pkg-config = 'false'
[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
[properties]
needs_exe_wrapper = true
''')
    build = work / 'mesa-ios-softpipe'
    if not (build / 'build.ninja').exists():
        options = ['platforms=', 'gallium-drivers=softpipe', 'vulkan-drivers=',
            'glx=disabled', 'egl=disabled', 'gbm=disabled', 'osmesa=true',
            'llvm=disabled', 'shared-glapi=enabled', 'gles1=disabled',
            'gles2=enabled', 'opengl=true', 'zstd=disabled', 'xmlconfig=disabled',
            'expat=disabled', 'shader-cache=disabled', 'valgrind=disabled',
            'libunwind=disabled', 'glvnd=disabled', 'video-codecs=', 'tools=',
            'gallium-vdpau=disabled', 'gallium-va=disabled', 'gallium-xa=disabled',
            'lmsensors=disabled', 'build-tests=false', 'b_ndebug=true']
        run('meson', 'setup', build, src, '--cross-file', cross,
            '--wrap-mode=nofallback', '--prefix=/redwood', '--libdir=lib',
            '--buildtype=release', *['-D' + opt for opt in options])
    run('ninja', '-C', build, '-j', '6')
    mesa = out / 'libOSMesa.8.dylib'
    shutil.copy2(build / 'src/gallium/targets/osmesa/libOSMesa.8.dylib', mesa)
    run('install_name_tool', '-id', '/redwood/lib/libOSMesa.8.dylib', mesa)
    symbols = subprocess.check_output(['nm', '-gU', str(mesa)], text=True)
    exports = out / 'exported-gl.txt'
    exports.write_text('\n'.join(line.split()[-1][1:] for line in symbols.splitlines()
        if line.split() and line.split()[-1].startswith('_gl')) + '\n')
    with (out / 'eagl_gen.h').open('w') as header:
        run(sys.executable, USER / 'userland/mkeagl.py', USER / 'userland/src/eagl_slots.txt',
            src / 'src/mapi/glapi/registry/gl.xml', exports,
            USER / 'userland/src/eagl.c', stdout=header)
    cc = ['xcrun', '--sdk', 'iphoneos', 'clang', '-arch', 'arm64',
          '-miphoneos-version-min=16.5', '-O1']
    frameworks = ['-framework', 'CoreFoundation', '-framework', 'IOKit']
    run(*cc, '-dynamiclib', '-install_name', '/redwood/lib/libredwood-gl.dylib',
        '-I' + str(out), *frameworks, '-framework', 'IOSurface', '-lobjc', mesa,
        '-o', out / 'libredwood-gl.dylib', USER / 'userland/src/eagl.c',
        USER / 'userland/src/gl_osmesa.c')
    run(*cc, '-dynamiclib', '-install_name', '/redwood/lib/libredwood.dylib',
        *frameworks, '-lobjc', '-o', out / 'libredwood.dylib', USER / 'userland/src/inject.c')
    run(*cc, *frameworks, mesa, '-o', out / 'gl_test', USER / 'userland/src/gl_test.c')
    run(*cc, '-Wall', '-Wextra', '-o', out / 'op13_cpu_check',
        USER / 'userland/src/op13_cpu_check.c')
    (out / 'BUILD.txt').write_text(f'Mesa {VERSION}\narchive_sha256={SHA}\n'
        f'normalized_patches_sha256={digest}\nSDK={sdk}\niOS minimum=16.5\n'
        'driver=softpipe (CPU software rasterizer; no Adreno acceleration)\n')
    print(f'Built iOS libraries and diagnostics: {out}')


if __name__ == '__main__':
    main()
