#!/usr/bin/env python3
"""Compile the tested iOS19 helpers with a local iPhoneOS SDK; never access USB."""
from pathlib import Path
import argparse
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True,help='Local RAM-root staging directory')
p.add_argument('--bootstrap',action='store_true',help='Also compile the static PID1 Cryptex handoff')
a=p.parse_args();root=Path(__file__).resolve().parents[1];repo=root.parents[1]
out=a.output.expanduser().resolve();(out/'lib').mkdir(parents=True,exist_ok=True);(out/'bin').mkdir(exist_ok=True)
sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip()
base=['xcrun','clang','-target','arm64e-apple-ios18.0','-isysroot',sdk,'-O2']
src27=repo/'source/ios27/userland/src'
units=[
 (root/'userland/inject-solarium19.c',out/'lib/libredwood26.dylib',['CoreFoundation','IOKit','IOSurface']),
 (root/'compat/edr-start.c',out/'lib/libedr-start.dylib',[]),
 (root/'compat/status-layout.m',out/'lib/libstatus-layout.dylib',['UIKit','CoreGraphics']),
 (root/'compat/slider-ios19.m',out/'lib/libslider-ios19.dylib',['UIKit']),
 (root/'compat/battery-probe.c',out/'bin/battery-probe',['CoreFoundation','IOKit'])]
for source,dest,frameworks in units:
 cmd=base.copy()
 if dest.suffix=='.dylib':cmd+=['-dynamiclib','-Wl,-install_name,/redwood/lib/'+dest.name]
 if source.suffix=='.m':cmd+=['-fobjc-arc']
 if source.name=='inject-solarium19.c':cmd+=['-I',str(src27),'-lobjc',str(src27/'IOMobileFramebuffer.tbd')]
 for framework in frameworks:cmd+=['-framework',framework]
 subprocess.run(cmd+[str(source),'-o',str(dest)],check=True)
 if 'IOKit' in frameworks:
  subprocess.run(['install_name_tool','-change','/System/Library/Frameworks/IOKit','/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit',str(dest)],check=True)
 subprocess.run(['codesign','-f','-s','-',str(dest)],check=True)
 subprocess.run(['codesign','--verify','--strict',str(dest)],check=True)
if a.bootstrap:
 mini=root/'libmini';obj=out/'obj';obj.mkdir(exist_ok=True)
 flags=['xcrun','clang','-target','arm64e-apple-ios18.0','-isysroot',sdk,'-Os','-ffreestanding','-fno-stack-protector','-fno-builtin','-I',str(mini)]
 for name in ['mini.c','start.S']:
  subprocess.run(flags+['-c',str(mini/name),'-o',str(obj/(Path(name).stem+'.o'))],check=True)
 subprocess.run(flags+['-static','-nostdlib','-Wl,-e,_start','-Wl,-pagezero_size,0x100000000',str(root/'compat/cryptex-links.c'),str(obj/'mini.o'),str(obj/'start.o'),'-o',str(out/'bin/cryptex-links')],check=True)
print('Compatibility helpers built locally; no phone contacted.')
