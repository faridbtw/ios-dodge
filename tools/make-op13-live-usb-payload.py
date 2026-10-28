#!/usr/bin/env python3
"""Make a separate seven-core iOS payload, reserving CPU0 for firmware USB."""
import argparse,hashlib,json,runpy
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--kernel',required=True,type=Path)
 p.add_argument('--output',required=True,type=Path)
 a=p.parse_args()
 source=ROOT/'experimental/slider/live-original-compositor-v1/payload.bin'
 blob=source.read_bytes()
 if hashlib.sha256(blob).hexdigest()!='e7965f334f65401b39411dad1ddd9674b6076f92f124542c3a31a12e7a9132b2':
  raise SystemExit('Working source payload changed; inspect before making a USB profile')
 api=runpy.run_path(str(ROOT/'tools/make-op13-test-payload.py'))
 entries=api['unpack'](blob)
 args=dict(t.split('=',1) for t in entries['cmdline'].rstrip(b'\0').decode().split())
 for k,v in {'cpus':'8','gl':'0','op13_batt':'4','slider':'1','ufs':'0','ufs_write':'0','var_store':'0','gpu':'0','adreno':'0','metal':'0','loader_return_after':'0'}.items():
  assert args.get(k)==v,k
 assert not any(k in args for k in ['adreno8','op13_audio','sm8750_test_reset_seconds'])
 args.update(cpus='7',bootcpu='6',op13_usb='1')
 assert args.pop('display_probe')=='0' # Omitted read-only probe defaults to disabled.
 cmd=' '.join(k+'='+v for k,v in args.items())
 assert len(cmd.encode())<=416,'Touch lease requires command line <=416 bytes'
 kernel=a.kernel.read_bytes()
 assert b'op13_usb\0' in kernel,'Kernel does not include the opt-in native-battery USB profile'
 entries['cmdline']=cmd.encode()+b'\0';entries['kernel']=kernel
 output=a.output.resolve();output.mkdir(parents=True,exist_ok=True)
 payload=api['pack'](entries);(output/'payload.bin').write_bytes(payload)
 reread=api['unpack'](payload)
 original=api['unpack'](blob)
 assert set(original)==set(reread)
 for k in original:
  if k not in ('kernel','cmdline'):assert original[k]==reread[k],k
 metadata={'experimental':True,'device_validated':False,'cpu_count_requested':7,'firmware_usb_cpu':0,'ios_boot_cpu':6,'native_battery_enabled':True,'slider':True,'renderer':'original CoreAnimation software','hardware_acceleration':False,'phone_partition_writes':False,'payload_sha256':hashlib.sha256(payload).hexdigest(),'source_payload_sha256':hashlib.sha256(blob).hexdigest(),'kernel_sha256':hashlib.sha256(kernel).hexdigest(),'cmdline':cmd,'cmdline_bytes':len(cmd),'changed_entries':['kernel','cmdline'],'all_ramdisk_entries_unchanged':True,'limitation':'Firmware fastboot diagnostics, not an iOS SSH or filesystem shell'}
 (output/'BUILD.json').write_text(json.dumps(metadata,indent=2)+'\n')
 print('Separate USB-service payload:',output/'payload.bin','command-line bytes:',len(cmd))

if __name__=='__main__':main()
