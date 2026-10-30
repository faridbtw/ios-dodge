#!/usr/bin/env python3
"""Build a private RAM payload, changing only kernel and fw/network."""
import argparse
import hashlib
import json
import os
import plistlib
from pathlib import Path, PurePosixPath
import runpy
import subprocess
import sys

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--network-file',type=Path)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--restore-wifid',action='store_true')
p.add_argument('--verify-internet',action='store_true')
p.add_argument('--wait-address-seconds',type=int,default=90)
a=p.parse_args()
assert 1 <= a.wait_address_seconds <= 600
root=Path(__file__).resolve().parents[3]
network=a.network_file.read_bytes() if a.network_file else None
if network is not None:
    lines=network.splitlines()
    assert len(lines)==2 and 1<=len(lines[0])<=32 and 8<=len(lines[1])<=63
    assert b'\0' not in network
out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);out.chmod(0o700)
tools=runpy.run_path(str(root/'tools/make-op13-test-payload.py'));apfs=tools['apfs']
digest=lambda data:hashlib.sha256(data).hexdigest()
blob=(root/'experimental/screen-brightness/control-v2/payload.bin').read_bytes()
assert digest(blob)=='175dfc60e6673fd1c5a5b636b1d5e60b2db263e9e0af3c8424830fd5df2785aa'
entries=tools['unpack'](blob);source_root=entries['ramdisk']
original=out/'original-root.apfs';original.write_bytes(source_root)
container=apfs.Container(str(original));volume=apfs.pick_volume(container,None)
tree=out/'root';tree.mkdir();manifest={};unchanged={}
for path,_,oid,_ in apfs.walk(volume,2):
    parts=PurePosixPath(path).parts[1:]
    assert parts and '..' not in parts
    inode=volume.inode(oid);dest=tree.joinpath(*parts)
    if inode.is_dir:dest.mkdir()
    elif inode.is_link:dest.symlink_to(os.fsdecode(inode.contents()))
    elif inode.is_reg:
        data=inode.contents();dest.write_bytes(data);unchanged[path]=digest(data)
    else:raise ValueError('Unsupported root inode')
    if not inode.is_link:dest.chmod(inode.mode&0o7777)
    manifest[path]=f'{inode.mode:o}\t{inode.owner}\t{inode.group}\t{path.lstrip("/")}'
container.close()
if not (tree/'fw').exists():
    (tree/'fw').mkdir();manifest['/fw']='40755\t0\t0\tfw'
changed=set()
if network is not None:
    (tree/'fw/network').write_bytes(network);(tree/'fw/network').chmod(0o600)
    manifest['/fw/network']='100600\t0\t0\tfw/network';changed.add('/fw/network')
elif (tree/'fw/network').exists():
    (tree/'fw/network').unlink();manifest.pop('/fw/network',None);changed.add('/fw/network')
if a.restore_wifid:
    stock=root/'diagnostics/20261024/apple-stock-jobs/System/Library/LaunchDaemons/com.apple.wifid.plist'
    job=plistlib.loads(stock.read_bytes())
    assert job['Label']=='com.apple.wifid' and job['Program']=='/usr/sbin/wifid'
    job.pop('_LimitLoadToDeviceTree',None)
    job.setdefault('EnvironmentVariables',{})['OS_ACTIVITY_DT_MODE']='YES'
    job['StandardOutPath']=job['StandardErrorPath']='/dev/console'
    path='/LaunchDaemons/com.apple.wifid.plist'
    (tree/path.lstrip('/')).write_bytes(plistlib.dumps(job,fmt=plistlib.FMT_BINARY))
    manifest[path]='100644\t0\t0\t'+path.lstrip('/')
    changed.add(path)
if a.verify_internet:
    path='/LaunchDaemons/com.redwood.wifi.plist'
    job=plistlib.loads((tree/path.lstrip('/')).read_bytes())
    assert job['Label']=='com.redwood.wifi'
    job['ProgramArguments']=['/redwood/bin/net_test','wifi','waitaddr','en0',str(a.wait_address_seconds),
                             'resolve','example.com','fetch','https://example.com/']
    (tree/path.lstrip('/')).write_bytes(plistlib.dumps(job,fmt=plistlib.FMT_BINARY))
    changed.add(path)
manifest_path=out/'root.manifest';manifest_path.write_text('\n'.join(manifest.values())+'\n')
image=out/'root.apfs'
subprocess.run([sys.executable,str(root/'source/hackios-xnu-userland/tools/apfs/mkapfs.py'),
               '-z','-o',str(image),f'redwood={tree},manifest={manifest_path}'],check=True)
container=apfs.Container(str(image));volume=apfs.pick_volume(container,None)
for path,expected in unchanged.items():
    if path in changed:continue
    hit=volume.lookup(path);assert hit and digest(volume.inode(hit[0]).contents())==expected,path
hit=volume.lookup('/fw/network')
assert (hit and volume.inode(hit[0]).contents()==network) if network is not None else hit is None
container.close()
entries['ramdisk']=image.read_bytes()
kernel=(root/'build/mac-xnu-peach-wifi-v2/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837').read_bytes()
assert b'op13_peach_join_given' in kernel or b'full Peach ath12k start' in kernel
entries['kernel']=kernel
cmdline=entries['cmdline'].rstrip(b'\0')+b' wifi=1'
assert len(cmdline)+1<=416
entries['cmdline']=cmdline+b'\0'
payload=tools['pack'](entries);(out/'payload.bin').write_bytes(payload);(out/'payload.bin').chmod(0o600)
meta=json.loads((root/'experimental/wifi/darwin-peach-v2/BUILD.json').read_text())
meta.update(payload_sha256=digest(payload),kernel_sha256=digest(kernel),root_sha256=digest(entries['ramdisk']),
            changed_entries=['kernel','ramdisk','cmdline'],modified_root_files=sorted(changed),
            ramdisk_and_cmdline_unchanged=False,cmdline_unchanged=False,
            cmdline=cmdline.decode(),cmdline_bytes=len(cmdline),modified_boot_args={'wifi':'1'},
            wifi_full_driver_scan_test=False,wifi_station_test=True,wifi_transport_probe_only=False,
            wifi_working=False,device_validated=False,private_network_configuration=network is not None)
meta['stock_wifid_restored']=a.restore_wifid
meta['internet_probe_requested']=a.verify_internet
(out/'BUILD.json').write_text(json.dumps(meta,indent=2)+'\n')
print('Private station payload built; verified original files outside '+', '.join(sorted(changed)))
