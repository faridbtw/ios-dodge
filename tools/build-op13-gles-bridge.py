#!/usr/bin/env python3
"""Build the matching EAGL/OSMesa bridge from source using local Mesa and SDK."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mesa', type=Path, required=True)
    p.add_argument('--mesa-source', type=Path, default=ROOT / 'build/mesa26-ios/mesa-26.2.5')
    p.add_argument('--sdk', type=Path, default=Path(os.environ.get('OP13_IOS_SDK',
                   str(ROOT / 'build/mac-xnu/deps/ios-sdks/iPhoneOS16.5.sdk'))))
    p.add_argument('--output', type=Path, default=ROOT / 'build/op13-gles')
    a = p.parse_args()
    assert a.sdk.is_dir() and a.mesa.is_file()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    mesa = out / 'libOSMesa.8.dylib'
    assert a.mesa.resolve() != mesa
    shutil.copy2(a.mesa, mesa)
    subprocess.run(['install_name_tool', '-id', '/redwood/lib/libOSMesa.8.dylib', str(mesa)], check=True)
    symbols = subprocess.check_output(['nm', '-gU', str(mesa)], text=True)
    exports = sorted({line.split()[-1][1:] for line in symbols.splitlines()
                      if line.split() and line.split()[-1].startswith('_gl')})
    assert 'glDrawArrays' in exports
    (out / 'exported-gl.txt').write_text('\n'.join(exports) + '\n')
    user = ROOT / 'source/hackios-xnu-userland/userland'
    xml = a.mesa_source / 'src/mesa/glapi/glapi/registry/gl.xml'
    with (out / 'eagl_gen.h').open('w') as generated:
        subprocess.run([sys.executable, str(user / 'mkeagl.py'), str(user / 'src/eagl_slots.txt'),
                        str(xml), str(out / 'exported-gl.txt'), str(user / 'src/eagl.c')],
                       stdout=generated, check=True)
    subprocess.run(['/usr/bin/clang', '-target', 'arm64-apple-ios16.5', '-isysroot', str(a.sdk),
                    '-O1', '-dynamiclib', '-install_name', '/redwood/lib/libredwood-gl.dylib',
                    '-I' + str(out), '-framework', 'CoreFoundation', '-framework', 'IOKit',
                    '-framework', 'IOSurface', '-lobjc', str(mesa), '-o', str(out / 'libredwood-gl.dylib'),
                    str(user / 'src/eagl.c'), str(user / 'src/gl_osmesa.c')], check=True)
    print(out / 'libredwood-gl.dylib')


if __name__ == '__main__':
    main()
