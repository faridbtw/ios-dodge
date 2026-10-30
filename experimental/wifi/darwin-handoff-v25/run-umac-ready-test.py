#!/usr/bin/env python3
"""Boot the verified Peach Wi-Fi/GLES iOS build entirely in RAM."""
import argparse
import fcntl
import hashlib
import json
import runpy
from pathlib import Path
import struct
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path)
p.add_argument('--serial', help='Connected Android phone serial')
p.add_argument('--check', action='store_true', help='Validate files without contacting the phone')
p.add_argument('--menu', action='store_true', help='Upload and wait for the phone Boot iOS button')
p.add_argument('--recover-idle-loader', action='store_true')
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
exp = Path(__file__).resolve().parent
build = root / 'experimental/wifi/darwin-station-v14'
meta = json.loads((build / 'BUILD.json').read_text())
loader = exp / 'project-boot-ios-uefi.img'
payload = build / 'payload.bin'
system = root / 'experimental/sileo/v1/system-sileo.apfs'
def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        while chunk := f.read(8 << 20): h.update(chunk)
    return h.hexdigest()
assert meta['experimental'] and meta['wifi_station_test']
assert not meta['production_ios_changed'] and not meta['phone_partition_writes']
assert digest(loader) == meta['loader_sha256'] == '2ee6c2a0def66dcb9ebe7a33268d3a1372202f1501de7219083fb647ab760955'
assert digest(payload) == meta['payload_sha256']
assert digest(system) == meta['system_sha256'] == 'ece8098938ebbc265c8c782bf705cf2f74747deea002ec3a5548cb4307909ed3'
assert all(word in meta['cmdline'].split() for word in ['cpus=7','bootcpu=1','ufs=0','ufs_write=0','var_store=0'])
assert meta['changed_entries'] == ['kernel','ramdisk','cmdline'] and meta['modified_boot_args']=={'wifi':'1'}
assert meta['modified_root_files']==['/LaunchDaemons/com.apple.wifid.plist','/LaunchDaemons/com.redwood.wifi.plist'] and meta['stock_wifid_restored']
entries=runpy.run_path(str(root/'tools/make-op13-test-payload.py'))['unpack'](payload.read_bytes())
assert hashlib.sha256(entries['kernel']).hexdigest()==meta['kernel_sha256']
assert entries['cmdline'].rstrip(b'\0').decode()==meta['cmdline'] and len(entries['cmdline'])<=416
args=dict(word.split('=',1) for word in meta['cmdline'].split())
required=dict(cpus='7',bootcpu='1',op13_usb='1',op13_batt='4',slider='1',slider_power='1',
              gl='1',metal='0',adreno8='23',adreno8_keep='1',adreno8_secure_preflight='4',
              ufs='0',ufs_write='0',var_store='0',cpufreq='0',loader_return_after='0',
              touch_steps='4',buttons_steps='5',wifi='1')
assert all(args.get(k)==v for k,v in required.items())
assert 'op13_audio' not in args and 'sm8750_test_reset_seconds' not in args
firmware=root/'diagnostics/20261029-wifi/native-reference/wifi-qdss-v5.private.bin'
assert digest(firmware)=='9f22e9beab0357f652f55e1f2c6ec5c8ed7869c42a03f2d1876ea86d88751ea9'
manifest=json.loads(loader.with_suffix('.json').read_text())
assert manifest['image_sha256']==meta['loader_sha256']
assert manifest['loader_executables_unchanged'] and manifest['other_inner_ffs_unchanged']
assert manifest['changed_inner_ffs']==['7bb28b99-61bb-11d5-9a5d-0090273fc14d']
base=(root/manifest['source']).resolve();assert base.is_relative_to(root.resolve())
packaging=json.loads(base.with_suffix('.json').read_text())
assert digest(base)==manifest['source_sha256']==packaging['image_sha256']
assert packaging['source_sha256']=='f26410ca510dcb7352f5c0b814ca435aaefe05bdc3ed1697ef5091b79d588cb8'
assert all(packaging[k] for k in ['other_ffs_unchanged','bootshim_unchanged','dtb_unchanged','ramdisk_and_signature_bytes_unchanged'])
assert all(manifest[k] for k in ['bootshim_unchanged','dtb_unchanged','ramdisk_and_signature_bytes_unchanged'])
if a.check:
    print(json.dumps(dict(package_verified=True,system_verified=True,firmware_verified=True,
                         loader_verified=True,device_contacted=False,phone_partition_writes=False),indent=2))
    raise SystemExit(0)
if not a.serial or not a.output:p.error('--serial and --output are required to boot')
boot_lock=(root/'build/gles-ram-boot.lock').open('a')
fcntl.flock(boot_lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
a.output.mkdir(parents=True, exist_ok=False)
a.output.chmod(0o700)
receipts = []
def command(args, timeout=30, check=True):
    argv = list(map(str, args))
    print('Running:', ' '.join(argv), flush=True)
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        receipts.append(dict(args=argv, exit=r.returncode, output=r.stdout+r.stderr))
        print(r.stdout+r.stderr, flush=True)
        if check: r.check_returncode()
        return r
    except subprocess.TimeoutExpired:
        receipts.append(dict(args=argv, timeout=timeout))
        raise
    finally:
        (a.output/'receipts.json').write_text(json.dumps(receipts, indent=2)+'\n')
def fb(*args, timeout=30, check=True):
    return command(['fastboot','-s','xnuloader',*args],timeout,check)
def capture(name):
    fb('oem','log'); fb('get_staged',a.output/name)

boot_args = ['/opt/homebrew/bin/python3', exp/'run-bundle.py', '--serial',a.serial,
             '--loader',loader,'--output',a.output/'boot','--action','link','--stage-wifi-firmware']
if a.recover_idle_loader: boot_args.append('--recover-idle-loader')
command(boot_args, 240)
fb('oem','op13-wifi-rails'); capture('rails.log')
command(['/opt/homebrew/bin/python3',root/'tools/run-op13-wifi-dma-steps.py',
         '--output',a.output/'dma','--steps','9','8','6','7'],180)
command(['/opt/homebrew/bin/python3',exp/'run-qmi.py','--output',a.output/'qmi'],240)
fb('oem','op13-wifi-transfer-prepare')
lease = a.output/'mhi-transfer.private.bin'
fb('get_staged',lease); lease.chmod(0o600)
raw = lease.read_bytes()
assert len(raw)==936 and struct.unpack_from('<Q',raw)[0]==0x4f50313357494649
assert struct.unpack_from('<I',raw,20)[0]==2
assert struct.unpack_from('<Q',raw,40)[0]==0x8000000
blocked = fb('oem','op13-wifi-mhi-export',check=False)
assert blocked.returncode and 'quiesced' in blocked.stdout+blocked.stderr
capture('transfer.log')
fb('oem','op13-dsi-brightness','ahb'); capture('brightness.log')
fb('stage',payload,timeout=60); fb('oem','ramdisk')
fb('stage',system,timeout=180)
fb('oem','op13-slider-probe'); capture('slider.log')
if a.menu:
    fb('oem','menu-ready')
    (a.output/'RESULT.json').write_text(json.dumps(dict(awaiting_phone_boot_button=True,
        wifi_transport_prepared=True,phone_partition_writes=False),indent=2)+'\n')
    print('iOS ready in RAM. Tap Boot iOS, or use Volume and Power.',flush=True)
    raise SystemExit(0)
fb('oem','sm8750-touch-arm'); fb('oem','run','0')
for delay in [5,15,45,60,60]:
    time.sleep(delay)
    fb('getvar','result')
    capture(f'ios-{delay}-{len(receipts)}.log')
logs = '\n'.join(path.read_text(errors='replace') for path in a.output.glob('ios-*.log'))
result = dict(wifi_working=False, ios_launch_sent=True, ownership_transferred=True,
              darwin_mhi_adopted='reserved Peach MHI state adopted' in logs,
              darwin_target_cap_verified='target-cap=1' in logs,
              full_driver_start_succeeded='full Peach ath12k start result=0' in logs,
              network_joined='THE NETWORK IS JOINED: frames can pass' in logs,
              ipv4_observed='waitaddr en0: an address' in logs,
              https_probe_succeeded='net_test: fetch https://example.com/: status 200' in logs,
              panic_detected='panic(cpu' in logs,
              phone_partition_writes=False, production_ios_changed=False)
result['wifi_working']=all(result[k] for k in ['network_joined','ipv4_observed','https_probe_succeeded']) and not result['panic_detected']
(a.output/'RESULT.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2),flush=True)
