#!/usr/bin/env python3
"""Native macOS build harness for the included XNU snapshot.

Dependency source revisions match xnu-deps/setup.sh. All generated files go
under build/mac-xnu; release kernels/payloads are never overwritten.
Actions: export (kernel headers), libs (firehose/pthread), build (kernel).
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path('/Users/faridbtw/Documents/Porting/OnePlus13-iOS-RAM')
SOURCE = ROOT / 'source/xnu-build-snapshot/xnu-8796.121.2'
SUPPORT = ROOT / 'source/hackios-xnu-kernel/xnu-deps'
WORK = ROOT / 'build/mac-xnu'
DEPS = WORK / 'deps'


def run(*args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def main():
    global SOURCE, WORK, DEPS
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['export', 'libs', 'build'])
    parser.add_argument('-j', type=int, default=4)
    parser.add_argument('--wifi-object', type=Path)
    parser.add_argument('--pthread-source', type=Path)
    parser.add_argument('--arch', choices=['arm64', 'arm64e'], default='arm64e')
    parser.add_argument('--source', type=Path, default=SOURCE)
    parser.add_argument('--work', type=Path, default=WORK)
    parser.add_argument('--deps', type=Path, default=DEPS)
    args = parser.parse_args()
    SOURCE, WORK, DEPS = args.source.resolve(), args.work.resolve(), args.deps.resolve()
    WORK.mkdir(parents=True, exist_ok=True)
    sdk_source = DEPS / 'ios-sdks/iPhoneOS16.5.sdk'
    required = [sdk_source / 'usr/include', DEPS / 'MacOSX13.3.sdk/System/Library/Frameworks/Kernel.framework/Headers/DriverKit',
        DEPS / 'AvailabilityVersions/availability.pl', DEPS / 'libdispatch/os/firehose_buffer_private.h',
        DEPS / 'libpthread/kern/kern_init.c']
    for path in required:
        if not path.exists(): raise SystemExit(f'Missing pinned build dependency: {path}')
    sdk = WORK / 'iPhoneOS16.5.xnu.sdk'
    for path in ['usr', 'usr/local/include/kernel/os', 'usr/local/include/os', 'usr/local/lib/kernel', 'usr/local/libexec']:
        (sdk / path).mkdir(parents=True, exist_ok=True)
    for name in ['System', 'usr/include', 'usr/lib']:
        target = sdk / name
        if not target.exists(): target.symlink_to(sdk_source / name, target_is_directory=True)
    for name in ['SDKSettings.plist', 'SDKSettings.json']:
        if (sdk_source / name).exists() and not (sdk / name).exists():
            (sdk / name).symlink_to(sdk_source / name)
    shutil.copy2(DEPS / 'AvailabilityVersions/availability.pl', sdk / 'usr/local/libexec/availability.pl')
    (sdk / 'usr/local/libexec/availability.pl').chmod(0o755)
    header = subprocess.run(['/usr/bin/unifdef', '-DOS_FIREHOSE_SPI=1', '-DKERNEL=1',
        str(DEPS / 'libdispatch/os/firehose_buffer_private.h')], capture_output=True)
    if header.returncode not in (0, 1): raise SystemExit(header.stderr.decode())
    (sdk / 'usr/local/include/kernel/os/firehose_buffer_private.h').write_bytes(header.stdout)
    shutil.copy2(DEPS / 'libdispatch/src/firehose/firehose_types.defs',
        sdk / 'usr/local/include/kernel/os/firehose_types.defs')
    binary = WORK / 'bin';binary.mkdir(exist_ok=True)
    if not (DEPS / 'iig-impl').exists(): (DEPS / 'iig-impl').symlink_to(SUPPORT / 'iig-impl', target_is_directory=True)
    iig = binary / 'iig';iig.write_text((SUPPORT / 'host/bin/iig').read_text());iig.chmod(0o755)
    for name in ['xnu-clang', 'xnu-clang++']:
        compiler = binary / name
        compiler.write_text('#!/usr/bin/env python3\nimport os,sys\n'
            f'compat={str(SUPPORT / "hostinc/xnu_clang_compat.h")!r}\n'
            'cxx=sys.argv[0].endswith("++")\n'
            'drop={"-enable-trivial-auto-var-init-zero-knowing-it-will-be-removed-from-clang",'
            '"-fapple-kext-vtable-verify","-Wno-vla-cxx-extension"}\n'
            'args=[a for a in sys.argv[1:] if a not in drop and not a.startswith("-Werror")]\n'
            'tool="/usr/bin/clang++" if cxx else "/usr/bin/clang"\n'
            'os.execv(tool,[tool,"-include",compat,"-target","arm64-apple-ios16.5","-w"]+'
            '(["-fno-use-cxa-atexit"] if cxx else [])+args)\n'.replace('arm64-apple-ios16.5', args.arch + '-apple-ios16.5'))
        compiler.chmod(0o755)
    # Static port uses the provided DriverKit stubs. Export only implemented KPIs.
    symbols_tool = binary / 'xnu-symbolset'
    symbols_tool.write_text('#!/bin/sh\nexec ' + str(WORK / 'obj/SETUP/kextsymboltool/kextsymboltool') + ' -sect \"$@\"\n')
    symbols_tool.chmod(0o755)
    nproc = binary / 'nproc'
    nproc.write_text('#!/bin/sh\necho ' + str(os.cpu_count()) + '\n')
    nproc.chmod(0o755)
    python = binary / 'python'
    if not python.exists(): python.symlink_to(sys.executable)
    host_sdk = subprocess.check_output(['/usr/bin/xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    host_mig = subprocess.check_output(['/usr/bin/xcrun', '--find', 'mig'], text=True).strip()
    # Use the real macOS SDK for host tools; the Linux xcrun shim always returns iOS.
    platform = WORK / 'Platforms/iPhoneOS.platform';platform.mkdir(parents=True, exist_ok=True)
    obj, dest, symbols = WORK / 'obj', WORK / 'dst', WORK / 'sym'
    env = os.environ.copy()
    env.setdefault('RC_DARWIN_KERNEL_VERSION', '25.6.0')
    env.update(XNUDEPS=str(DEPS), XNU_SDK_PATH=str(sdk), XNU_SDK_VERSION='16.5',
        XNU_PLATFORM_PATH=str(platform), PATH=str(binary) + os.pathsep + env['PATH'])
    flags = [f'OBJROOT={obj}', f'DSTROOT={dest}', f'SYMROOT={symbols}', f'SDKROOT={sdk}',
        f'SDKROOT_RESOLVED={sdk}', f'HOST_SDKROOT={host_sdk}', f'HOST_SDKROOT_RESOLVED={host_sdk}',
        f'PLATFORMPATH={platform}', 'SDKVERSION=16.5', 'ARCH_CONFIGS=ARM64', 'MACHINE_CONFIGS=BCM2837',
        'KERNEL_CONFIGS=DEVELOPMENT', 'PLATFORM=iPhoneOS',
        'SYSCTL_HW_PHYSICALCPU=' + str(os.cpu_count()), 'SYSCTL_HW_LOGICALCPU=' + str(os.cpu_count()),
        'MEMORY_SIZE=' + subprocess.check_output(['/usr/sbin/sysctl', '-n', 'hw.memsize'], text=True).strip(),
        'KERNEL_BUILDS_IN_PARALLEL=1', 'ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=' + args.arch,
        'BUILD_WERROR=0', 'BUILD_LTO=0', 'DO_CTFMERGE=0', 'HOST_CC=/usr/bin/clang',
        'HOST_CXX=/usr/bin/clang++', 'HOST_FLEX=/usr/bin/flex', 'HOST_BISON=/usr/bin/bison',
        'HOST_GM4=/usr/bin/m4', f'CC={binary / "xnu-clang"}', f'CXX={binary / "xnu-clang++"}',
        'IIG=' + subprocess.check_output(['/usr/bin/xcrun', '--find', 'iig'], text=True).strip(), f'KEXT_CREATE_SYMBOL_SET={binary / "xnu-symbolset"}', f'MIG={host_mig}', 'LOGCOLORS=n']
    if args.action == 'export':
        run('/usr/bin/make', '-j' + str(args.j), *flags, 'exporthdrs', cwd=SOURCE, env=env)
        return
    if args.action == 'libs':
        headers = obj / 'EXPORT_HDRS'
        if not (headers / 'osfmk').exists(): raise SystemExit('Run export first')
        objects = WORK / 'libraries';objects.mkdir(exist_ok=True)
        platform_headers = objects / 'platform-headers';platform_headers.mkdir(exist_ok=True)
        shutil.copy2(sdk_source / 'usr/include/TargetConditionals.h', platform_headers / 'TargetConditionals.h')
        includes = ['-I' + str(headers / part) for part in ['osfmk', 'bsd', 'libkern', 'pexpert', 'iokit', 'security', 'libsa']]
        common = [str(binary / 'xnu-clang'), '-arch', args.arch, '-c', '-Os', '-mkernel', '-nostdinc',
            '-fno-builtin', '-isystem', subprocess.check_output(['/usr/bin/clang', '-print-resource-dir'], text=True).strip() + '/include', '-DKERNEL=1', '-DKERNEL_PRIVATE=1', '-DPRIVATE=1',
            '-D__DARWIN_64_BIT_INO_T=1', '-DARM64_BOARD_CONFIG_BCM2837',
            *includes, '-I' + str(SOURCE / 'EXTERNAL_HEADERS'), '-I' + str(platform_headers)]
        dispatch = DEPS / 'libdispatch'
        firehose = objects / 'firehose_buffer.o'
        run(*common, '-UXNU_KERNEL_PRIVATE', '-DDISPATCH_USE_DTRACE=0',
            '-DOS_ATOMIC_CONFIG_MEMORY_ORDER_DEPENDENCY=1', '-DOS_ATOMIC_CONFIG_STARVATION_FREE_ONLY=0',
            '-DOS_FIREHOSE_SPI=1', *['-I' + str(dispatch / part) for part in ['', 'src', 'src/firehose']],
            '-I' + str(SOURCE / 'libkern/firehose'), '-I' + str(headers / 'libkern/os'),
            '-I' + str(sdk / 'usr/local/include/kernel'), '-o', firehose,
            dispatch / 'src/firehose/firehose_buffer.c', env=env)
        run('/usr/bin/libtool', '-static', '-o', sdk / 'usr/local/lib/kernel/libfirehose_kernel.a', firehose)
        pthread = args.pthread_source.resolve() if args.pthread_source else DEPS / 'libpthread';parts = []
        for name in ['kern_init', 'kern_support', 'kern_synch']:
            output = objects / ('pthread_' + name + '.o');parts.append(output)
            run(*common, '-fno-common', '-Wno-int-conversion', '-UXNU_KERNEL_PRIVATE', '-I' + str(SOURCE / 'bsd'), '-I' + str(SOURCE / 'osfmk/libsa'), '-I' + str(SOURCE / 'osfmk'), '-I' + str(obj / 'DEVELOPMENT_ARM64_BCM2837/config'), *['-I' + str(obj / 'DEVELOPMENT_ARM64_BCM2837' / part / 'DEVELOPMENT') for part in ['bsd', 'osfmk', 'iokit']], '-D__PTHREAD_EXPOSE_INTERNALS__=1',
                '-Dpthread_kern=pthread_kext_kern', '-Dcurrent_uthread=pthread_kext_current_uthread',
                '-I' + str(SUPPORT / 'build/pthread/inc'), '-I' + str(pthread),
                '-I' + str(pthread / 'include'), '-I' + str(pthread / 'private'),
                '-o', output, pthread / 'kern' / (name + '.c'), env=env)
        run('/usr/bin/libtool', '-static', '-o', sdk / 'usr/local/lib/kernel/libpthread_kext.a', *parts)
        return
    if args.wifi_object:
        flags.append('REDWOOD_OBJECTS=' + str(args.wifi_object.resolve()))
    run('/usr/bin/make', '-j' + str(args.j), *flags, cwd=SOURCE, env=env)


if __name__ == '__main__': main()
