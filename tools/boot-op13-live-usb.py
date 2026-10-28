#!/usr/bin/env python3
"""RAM-boot the opt-in seven-core USB-service profile and capture live logs."""
import argparse,hashlib,json,runpy,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--serial',required=True)
 p.add_argument('--payload',required=True,type=Path)
 p.add_argument('--output',required=True,type=Path)
 p.add_argument('--observe-seconds',type=int,default=90)
 a=p.parse_args();payload=a.payload.resolve();image=ROOT/'experimental/uefi-turtle/stable-overlay-v11/dodge-ios-uefi.img'
 meta=json.loads((payload.parent/'BUILD.json').read_text())
 assert hashlib.sha256(payload.read_bytes()).hexdigest()==meta['payload_sha256']
 image_meta=json.loads(image.with_suffix('.json').read_text())
 assert hashlib.sha256(image.read_bytes()).hexdigest()==image_meta['image_sha256']
 assert image_meta['device_validated'] and image_meta['usb_reconnect']
 api=runpy.run_path(str(ROOT/'tools/make-op13-test-payload.py'));entries=api['unpack'](payload.read_bytes())
 boot=dict(t.split('=',1) for t in entries['cmdline'].rstrip(b'\0').decode().split())
 for k,v in {'cpus':'7','bootcpu':'6','op13_usb':'1','op13_batt':'4','gl':'0','ufs':'0','ufs_write':'0','var_store':'0','gpu':'0','adreno':'0','metal':'0','loader_return_after':'0'}.items():assert boot.get(k)==v,k
 assert not any(k in boot for k in ['adreno8','op13_audio','sm8750_test_reset_seconds'])
 out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);receipts=[]
 def command(argv,timeout=10,check=True):
  start=time.monotonic()
  try:
   r=subprocess.run(argv,capture_output=True,text=True,timeout=timeout)
   receipts.append({'args':argv,'exit':r.returncode,'seconds':time.monotonic()-start,'output':r.stdout+r.stderr})
   if check:r.check_returncode()
   return r.stdout+r.stderr
  except subprocess.TimeoutExpired:
   receipts.append({'args':argv,'timeout':timeout});raise
  finally:(out/'receipts.json').write_text(json.dumps(receipts,indent=2)+'\n')
 def fb(args,serial='xnuloader',timeout=10):
  allowed=(args[:1] in [['getvar'],['get_staged']] or args in [['boot',str(image)],['stage',str(payload)],['stage',str(ROOT/'system.apfs')],['oem','ramdisk'],['oem','op13-slider-probe'],['oem','sm8750-touch-arm'],['oem','run','0'],['oem','log']])
  assert allowed,'Only RAM boot/staging and read-only diagnostics are supported'
  return command(['fastboot','-s',serial,*args],timeout)
 assert command(['adb','-s',a.serial,'shell','getprop ro.board.platform']).strip()=='sun'
 assert command(['adb','-s',a.serial,'shell','getprop sys.boot_completed']).strip()=='1'
 print('Booting tested iOS UEFI into RAM',flush=True)
 command(['adb','-s',a.serial,'reboot','bootloader'])
 for _ in range(45):
  try:
   if 'sun' in fb(['getvar','product'],a.serial,2):break
  except (subprocess.TimeoutExpired,subprocess.CalledProcessError):pass
  time.sleep(.5)
 else:raise SystemExit('Matched Android bootloader not available')
 fb(['boot',str(image)],a.serial,30)
 for _ in range(40):
  try:
   if 'xnuloader' in fb(['getvar','product'],timeout=3):break
  except (subprocess.TimeoutExpired,subprocess.CalledProcessError):pass
  time.sleep(.5)
 else:raise SystemExit('iOS firmware USB did not enumerate')
 session=fb(['getvar','usb-session'])
 fb(['stage',str(payload)],timeout=30);fb(['oem','ramdisk'])
 print('Uploading iOS system image into RAM',flush=True)
 fb(['stage',str(ROOT/'system.apfs')],timeout=150)
 fb(['oem','op13-slider-probe']);fb(['oem','sm8750-touch-arm'],timeout=20)
 fb(['oem','run','0']);print('iOS starting on seven cores; checking live USB',flush=True)
 start=time.monotonic();samples=[]
 while time.monotonic()-start<a.observe_seconds:
  time.sleep(5)
  elapsed=round(time.monotonic()-start,1)
  try:
   result=fb(['getvar','result'],timeout=4)
   current_session=fb(['getvar','usb-session'],timeout=4)
   assert current_session.split('usb-session:')[-1].splitlines()[0]==session.split('usb-session:')[-1].splitlines()[0]
   fb(['oem','log'],timeout=5);log=out/('live-%03d.log'%len(samples));fb(['get_staged',str(log)],timeout=20)
   sample={'seconds':elapsed,'result':result,'log':log.name,'log_bytes':log.stat().st_size}
   samples.append(sample);(out/'SAMPLES.json').write_text(json.dumps(samples,indent=2)+'\n')
   print('Live USB sample at',elapsed,'seconds:',log.stat().st_size,'log bytes',flush=True)
   if 'result: running ' not in result and 'result: running\n' not in result:
    print('Kernel is no longer running; inspect logs',flush=True);break
  except (subprocess.TimeoutExpired,subprocess.CalledProcessError,AssertionError) as e:
   (out/'ERROR.txt').write_text(str(e)+'\n');print('Live USB verification failed:',e,flush=True);break
 good=bool(samples) and samples[-1]['seconds']>=a.observe_seconds-8 and all('result: running' in s['result'] for s in samples)
 (out/'RESULT.json').write_text(json.dumps({'live_usb_observed':good,'samples':len(samples),'same_firmware_session':bool(samples),'requested_ios_cpu_count':7,'firmware_usb_cpu':0,'phone_partition_writes':False,'ios_ui_confirmed':False,'battery_ui_confirmed':False,'last_sample_seconds':samples[-1]['seconds'] if samples else None},indent=2)+'\n')
 if not good:raise SystemExit('Live USB not established; inspect captured logs')
 print('Live firmware USB access remained responsive while the kernel ran.',flush=True)

if __name__=='__main__':main()
