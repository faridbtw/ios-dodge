#!/usr/bin/env python3
"""Run a90s CPU6 slider diagnostic in the already verified idle RAM loader."""
import argparse,hashlib,json,runpy,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--payload',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();payload=a.payload.resolve()
manifest=json.loads((payload.parent/'BUILD.json').read_text())
assert manifest['payload_sha256']==hashlib.sha256(payload.read_bytes()).hexdigest()
entries=runpy.run_path(str(ROOT/'tools/make-op13-test-payload.py'))['unpack'](payload.read_bytes())
boot=dict(t.split('=',1) for t in entries['cmdline'].rstrip(b'\0').decode().split())
for k,v in {'cpus':'1','bootcpu':'6','slider':'1','slider_power':'1','op13_batt':'0','gl':'0','gpu':'0','adreno':'0','ufs_write':'0','var_store':'0','sm8750_test_reset_seconds':'90'}.items():assert boot[k]==v,k
out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);receipts=[]
def command(args,timeout=10,check=True):
 try:
  r=subprocess.run(args,capture_output=True,text=True,timeout=timeout)
  receipts.append({'args':args,'exit':r.returncode,'output':r.stdout+r.stderr})
  if check:r.check_returncode()
  return r.stdout+r.stderr
 except subprocess.TimeoutExpired:
  receipts.append({'args':args,'timeout':timeout});raise
 finally:(out/'receipts.json').write_text(json.dumps(receipts,indent=2)+'\n')
def fb(args,timeout=10):return command(['fastboot','-s','xnuloader',*args],timeout)
assert 'xnuloader' in fb(['getvar','product'])
assert 'result: idle' in fb(['getvar','result'])
fb(['oem','op13-slider-probe'])
fb(['stage',str(payload)],20);fb(['oem','ramdisk'])
fb(['stage',str(ROOT/'system.apfs')],120)
fb(['oem','sm8750-touch-arm'],15);fb(['oem','run','0'])
print('Timed native slider test started; cold reset scheduled at90s',flush=True)
start=time.monotonic();last=None
for index in range(75):
 if time.monotonic()-start>87:break
 time.sleep(1)
 try:
  fb(['getvar','result'],2);fb(['oem','log'])
  path=out/f'live-{index:03d}.log';fb(['get_staged',str(path)])
  last=path
  lines=[s for s in path.read_text(errors='replace').splitlines() if s.startswith('op13-slider:') or 'redwood: slider:' in s or 'alert slider is' in s]
  text='\n'.join(lines)
  if not (out/'observations.txt').exists() or text!=(out/'observations.txt').read_text():
   (out/'observations.txt').write_text(text);print('\n'.join(lines[-8:]),flush=True)
 except (subprocess.TimeoutExpired,subprocess.CalledProcessError):pass
if last:(out/'last-kernel.log').write_bytes(last.read_bytes())
print('Capture complete; waiting for the scheduled cold reset',flush=True)
