#!/usr/bin/env python3
"""Add the optional alert slider to a copy of the validated battery/gl=0 build."""
import argparse,hashlib,json,runpy,shutil,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
APFS=ROOT/'source/hackios-xnu-userland/tools/apfs'
sys.path.insert(0,str(APFS));import apfs
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--calibration',type=Path,required=True)
p.add_argument('--probe',action='store_true',help='CPU6/one core,90s cold reset, no ADSP startup')
a=p.parse_args()
cal=[int(s.strip()) for s in a.calibration.read_text().strip().split(',')]
assert len(cal)==9 and all(-32768<=s<=32767 for s in cal)
source=ROOT/'experimental/battery/live-original-compositor-v1'
base=source/'payload.bin'
assert hashlib.sha256(base.read_bytes()).hexdigest()=='cc934945f1fd93a526a16ef27582c7be6b59c76c3537ef05f1d10fbed12d993a'
out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
shutil.copytree(source/'root',out/'root',symlinks=True)
shutil.copy2(source/'root.manifest',out/'root.manifest')
changed={'/lib/libredwood.dylib','/lib/libredwood-sb.dylib'}
for name in changed:shutil.copy2(ROOT/'experimental/slider/userland'/Path(name).name,out/'root'/name.lstrip('/'))
for path in (source/'root').rglob('*'):
 rel=path.relative_to(source/'root')
 if path.is_file() and not path.is_symlink() and '/'+str(rel) not in changed:
  assert path.read_bytes()==(out/'root'/rel).read_bytes(),str(rel)
image=out/'root.apfs'
subprocess.run([sys.executable,str(APFS/'mkapfs.py'),'-z','-o',str(image),f'redwood={out/"root"},manifest={out/"root.manifest"}'],check=True)
c=apfs.Container(str(image));v=apfs.pick_volume(c,None)
for name,_,oid,_ in apfs.walk(v,2):
 inode=v.inode(oid)
 if inode.is_reg:assert inode.contents()==(out/'root'/name.lstrip('/')).read_bytes(),name
m=runpy.run_path(str(ROOT/'tools/make-op13-test-payload.py'))
entries=m['unpack'](base.read_bytes())
entries['ramdisk']=image.read_bytes()
entries['kernel']=(ROOT/'build/mac-xnu/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837').read_bytes()
boot=dict(t.split('=',1) for t in entries['cmdline'].rstrip(b'\0').decode().split())
boot.update(slider='1',slider_power='1',slider_cal=','.join(map(str,cal)))
if a.probe:
 boot.update(cpus='1',bootcpu='6',op13_batt='0',loader_return_after='120',sm8750_test_reset_seconds='90')
 for name in ['epoch','display_probe']:boot.pop(name,None)
cmdline=' '.join(k+'='+v for k,v in boot.items())
assert len(cmdline)<=416,len(cmdline)
entries['cmdline']=(cmdline+'\0').encode()
payload=m['pack'](entries);assert m['unpack'](payload)==entries
(out/'payload.bin').write_bytes(payload)
loader=ROOT/'experimental/slider/sensor-probe-v5/dodge-slider-probe.img'
(out/'BUILD.json').write_text(json.dumps({'experimental':True,'device_validated':False,'slider':True,'mapping':'bottom ring; middle silent; top DND','live_battery_enabled':not a.probe,'hardware_gpu_acceleration':False,'renderer':'CoreAnimation software','source_payload_sha256':hashlib.sha256(base.read_bytes()).hexdigest(),'payload_sha256':hashlib.sha256(payload).hexdigest(),'kernel_sha256':hashlib.sha256(entries['kernel']).hexdigest(),'loader':str(loader.relative_to(ROOT)),'loader_sha256':hashlib.sha256(loader.read_bytes()).hexdigest(),'cmdline':cmdline,'modified_root_files':sorted(changed),'phone_partition_writes':False},indent=2)+'\n')
print(out/'payload.bin');print('cmdline bytes:',len(cmdline))
