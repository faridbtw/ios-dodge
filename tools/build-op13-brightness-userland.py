#!/usr/bin/env python3
"""Compile the screen brightness agent and scoped Control Center wrapper."""
import argparse,os,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--sdk',type=Path,required=True)
 p.add_argument('--output',type=Path,default=ROOT/'build/screen-brightness')
 p.add_argument('--cc',default=os.environ.get('CC','/usr/bin/clang'))
 a=p.parse_args();assert a.sdk.is_dir()
 a.output.mkdir(parents=True,exist_ok=True)
 src=ROOT/'source/screen-brightness'
 common=[a.cc,'-target','arm64-apple-ios16.5','-isysroot',str(a.sdk),'-Os','-Wall','-Wextra','-Werror']
 agent=a.output/'op13-screen-agent';wrapper=a.output/'libop13-screen-cc.dylib'
 subprocess.run(common+[str(src/'brightness.c'),'-framework','CoreFoundation','-o',str(agent)],check=True)
 subprocess.run(common+['-fblocks','-dynamiclib','-install_name','/redwood/lib/libop13-screen-cc.dylib',str(src/'cc-brightness.c'),'-lobjc','-o',str(wrapper)],check=True)
 for binary in (agent,wrapper):subprocess.run(['codesign','-s','-',str(binary)],check=True)
 print(a.output)
if __name__=='__main__':main()
