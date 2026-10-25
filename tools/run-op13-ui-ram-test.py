#!/usr/bin/env python3
"""Timed eight-core/software-rendering regression, using only the diagnostic RAM boot route."""
import argparse,subprocess,time,json,hashlib,runpy
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--serial',required=True);p.add_argument('--payload',required=True,type=Path);p.add_argument('--output',required=True,type=Path)
p.add_argument('--reuse-returned-loader', action='store_true', help='Reuse the resident root after a completed timed UI test')
p.add_argument('--original-compositor', action='store_true', help='Test the original CoreAnimation software renderer instead of Mesa')
a=p.parse_args(); payload=a.payload.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
entries=runpy.run_path(str(ROOT/'tools/make-op13-test-payload.py'))['unpack'](payload.read_bytes())
boot=dict(x.split('=',1) for x in entries['cmdline'].rstrip(b'\0').decode().split())
for key,value in {'cpus':'8','gicr_stride':'0x40000','gpu':'0','adreno':'0','metal':'0','ufs':'0','ufs_write':'0','var_store':'0','gl':'0' if a.original_compositor else '1'}.items():
 if boot.get(key)!=value:raise SystemExit('Unexpected boot parameter '+key)
if 'adreno8' in boot:raise SystemExit('This regression does not run the unfinished GPU probe')
seconds=int(boot['loader_return_after']);assert 60<=seconds<=180
loader=ROOT/'experimental/adreno120/dodge-display-query.img'
assert hashlib.sha256(loader.read_bytes()).hexdigest()=='29f2d04a2db4a684d6436564e3f3a916fbde01bb7081a9604882665b4052a86e'
receipts=[]
def cmd(argv,timeout=8):
 try:
  r=subprocess.run(argv,capture_output=True,text=True,timeout=timeout)
  receipts.append({'command':argv,'exit':r.returncode,'output':r.stdout+r.stderr});r.check_returncode();return r.stdout+r.stderr
 finally:(out/'receipts.json').write_text(json.dumps(receipts,indent=2)+'\n')
def fb(args,timeout=8,serial='xnuloader'):return cmd(['fastboot','-s',serial,*args],timeout)
if a.reuse_returned_loader:
 assert 'xnuloader' in fb(['getvar','product'])
 assert 'result: returned ' in fb(['getvar','result'])
else:
 identity=cmd(['adb','-s',a.serial,'shell','getprop ro.board.platform; getprop sys.boot_completed'])
 assert identity.splitlines()[0]=='sun'
 for _ in range(30):
  if cmd(['adb','-s',a.serial,'shell','getprop sys.boot_completed']).strip()=='1':break
  time.sleep(1)
 else:raise SystemExit('Android startup incomplete')
 cmd(['adb','-s',a.serial,'reboot','bootloader'])
 for _ in range(40):
  time.sleep(.5)
  try:
   if 'sun' in fb(['getvar','product'],2,a.serial):break
  except (subprocess.TimeoutExpired,subprocess.CalledProcessError):pass
 else:raise SystemExit('Android bootloader unavailable')
 fb(['boot',str(loader)],20,a.serial)
 for _ in range(40):
  time.sleep(.5)
  try:
   if 'xnuloader' in fb(['getvar','product'],5):break
  except (subprocess.TimeoutExpired,subprocess.CalledProcessError):pass
 else:raise SystemExit('RAM loader unavailable')
assert '0x03f00000' in fb(['getvar','max-download-size'])
fb(['stage',str(payload)],15)
if not a.reuse_returned_loader:
 fb(['oem','ramdisk']);fb(['stage',str(ROOT/'system.apfs')],120)
fb(['oem','sm8750-touch-arm'],15);fb(['oem','run','0'])
print(f'iOS regression running in RAM; expected loader return after {seconds}s',flush=True)
# CPU0 belongs to XNU during an eight-core run, so USB resumes at timed return.
for _ in range((seconds+9)//5):time.sleep(5)
for _ in range(20):
 try:
  result=fb(['getvar','result'],3);break
 except (subprocess.TimeoutExpired,subprocess.CalledProcessError):time.sleep(1)
else:raise SystemExit('No timed loader return; no success claimed')
(out/'loader-result.txt').write_text(result)
fb(['oem','log']);fb(['get_staged',str(out/'kernel.log')])
fb(['oem','screenshot']);fb(['get_staged',str(out/'screen.raw')],15)
log=(out/'kernel.log').read_text(errors='replace')
interesting=[l for l in log.splitlines() if any(s in l for s in ['op13_cpu_check:','gl_test:','fb_rate_test:','PANIC','panic(cpu'])]
(out/'observations.txt').write_text('\n'.join(interesting)+'\n')
print('\n'.join(interesting[-35:]),flush=True)
print('Returned to RAM loader; original phone partitions unchanged',flush=True)
