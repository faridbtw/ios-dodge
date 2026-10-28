#!/usr/bin/env python3
"""Configure/build the separately prepared Mesa26 Adreno830 iOS runtime.
Prepare with tools/prepare-op13-gles-mesa.py; see source/mesa26-op13.
"""
from pathlib import Path
import subprocess,os,re,hashlib
r=Path(__file__).resolve().parents[1]
work=r/'build/mesa26-ios'
src=work/'mesa-26.2.5'
assert hashlib.sha256((work/'mesa-26.2.5.tar.xz').read_bytes()).hexdigest()=='794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5'
assert (src/'.op13-prepared').exists() and not list(src.rglob('*.rej'))
sdk=os.environ.get('OP13_IOS_SDK', str(r/'build/mac-xnu/deps/ios-sdks/iPhoneOS16.5.sdk'))
assert Path(sdk).is_dir(), 'Supply the pinned iPhoneOS16.5 SDK with OP13_IOS_SDK'
cross=work/'ios-cross.ini';cross.write_text(f'''[binaries]
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
options=['platforms=','gallium-drivers=softpipe,freedreno','freedreno-kmds=msm','vulkan-drivers=','glx=disabled','egl=disabled','gbm=disabled','osmesa=true','llvm=disabled','shared-glapi=enabled','gles1=disabled','gles2=enabled','opengl=true','zstd=disabled','xmlconfig=disabled','expat=disabled','shader-cache=disabled','valgrind=disabled','libunwind=disabled','glvnd=disabled','video-codecs=','tools=','gallium-vdpau=disabled','gallium-va=disabled','gallium-xa=disabled','lmsensors=disabled','build-tests=false']
supported=set(re.findall(r"option\(\s*'([^']+)'",(src/'meson.options').read_text()))
skipped=[o for o in options if o.split('=')[0] not in supported];print('Removed upstream options:',skipped)
options=[o for o in options if o.split('=')[0] in supported]
env=os.environ.copy();env['PATH']=str(work/'venv/bin')+os.pathsep+'/opt/homebrew/opt/bison/bin'+os.pathsep+env['PATH']
subprocess.run(['meson','setup',str(work/'mesa-ios-v2'),str(src),'--cross-file',str(cross),'--wrap-mode=nofallback','--prefix=/redwood','--libdir=lib','--buildtype=release','-Db_ndebug=true',*['-D'+o for o in options]],env=env,check=True)

subprocess.run(['ninja','-C',str(work/'mesa-ios-v2'),'-j','6','src/gallium/targets/osmesa/libOSMesa.8.dylib'],env=env,check=True)
