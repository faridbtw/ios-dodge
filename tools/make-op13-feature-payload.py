#!/usr/bin/env python3
"""Add confirmed Sileo jobs and screen-brightness helpers to a local RAM payload.

Apple/system/firmware inputs are supplied locally and never downloaded here.
The kernel must already contain the native torch and screen drivers. The RAM
root is unpacked and reconstructed with its original ownership and modes.
"""
import argparse,hashlib,json,os,runpy,subprocess,sys
from pathlib import Path,PurePosixPath
ROOT=Path(__file__).resolve().parents[1]
APFS=ROOT/'source/hackios-xnu-userland/tools/apfs'
sys.path.insert(0,str(APFS));import apfs
def sha(data):return hashlib.sha256(data).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--base-payload',type=Path,required=True)
 p.add_argument('--kernel',type=Path,required=True)
 p.add_argument('--system',type=Path,required=True)
 p.add_argument('--brightness-userland',type=Path,required=True)
 p.add_argument('--output',type=Path,required=True)
 p.add_argument('--sileo',action='store_true',help='System was prepared with the Sileo bootstrap')
 a=p.parse_args();meta=json.loads((a.base_payload.parent/'BUILD.json').read_text())
 original=a.base_payload.read_bytes();assert sha(original)==meta['payload_sha256']
 api=runpy.run_path(str(ROOT/'tools/make-op13-test-payload.py'));entries=api['unpack'](original)
 kernel=a.kernel.read_bytes()
 for marker in (b'op13-screen: native DCS51 control v1 ready',b'op13-torch: four-channel control v1 mask0f cap32mA sleeping-worker'):
  assert marker in kernel,marker
 a.output.mkdir(parents=True,exist_ok=False)
 old=a.output/'original-root.apfs';old.write_bytes(entries['ramdisk'])
 volume=apfs.pick_volume(apfs.Container(str(old)),None)
 tree=a.output/'root';tree.mkdir();manifest=[]
 for path,_,oid,_ in apfs.walk(volume,2):
  relative=PurePosixPath(path).parts[1:];assert '..' not in relative
  inode=volume.inode(oid);target=tree.joinpath(*relative)
  if inode.is_dir:target.mkdir()
  elif inode.is_link:target.symlink_to(os.fsdecode(inode.contents()))
  elif inode.is_reg:target.write_bytes(inode.contents())
  else:raise ValueError('Unsupported inode '+path)
  if not inode.is_link:target.chmod(inode.mode&0o7777)
  manifest.append(f'{inode.mode:o}\t{inode.owner}\t{inode.group}\t{path.lstrip("/")}')
 for relative,name in [('bin/op13-screen-agent','op13-screen-agent'),('lib/libop13-screen-cc.dylib','libop13-screen-cc.dylib')]:
  target=tree/relative;target.write_bytes((a.brightness_userland/name).read_bytes());target.chmod(0o755)
  manifest=[line for line in manifest if line.split('\t')[-1]!=relative]
  manifest.append(f'100755\t0\t0\t{relative}')
 conf=tree/'etc/inject.conf';content=conf.read_text()
 if '/redwood/lib/libop13-screen-cc.dylib SpringBoard' not in content:
  assert content.count('/redwood/lib/libredwood-sb.dylib SpringBoard')==1
  content=content.replace('/redwood/lib/libredwood-sb.dylib SpringBoard','/redwood/lib/libop13-screen-cc.dylib SpringBoard')
 assert len(content.encode())<512;conf.write_text(content)
 assert (tree/'lib/libredwood-sb.dylib').is_file()
 jobs=tree/'etc/rc.jobs';content=jobs.read_text()
 if a.sileo and '/var/jb/bin/sh /var/jb/prep.sh' not in content:
  content='/var/jb/bin/sh /var/jb/prep.sh\n/var/jb/usr/bin/dpkg-query -W org.coolstar.sileo ellekit\n'+content
 if '/redwood/bin/op13-screen-agent' not in content:content+='\n/redwood/bin/op13-screen-agent &\n'
 jobs.write_text(content)
 manifest_path=a.output/'root.manifest';manifest_path.write_text('\n'.join(manifest)+'\n')
 ramdisk=a.output/'root.apfs'
 subprocess.run([sys.executable,str(APFS/'mkapfs.py'),'-z','-o',str(ramdisk),f'System={tree},manifest={manifest_path}'],check=True)
 checked=apfs.pick_volume(apfs.Container(str(ramdisk)),None);count=0
 for path,_,oid,_ in apfs.walk(checked,2):
  inode=checked.inode(oid)
  if inode.is_reg:assert inode.contents()==(tree/path.lstrip('/')).read_bytes(),path;count+=1
 args=entries['cmdline'].rstrip(b'\0').decode().split()
 args=[value for value in args if not value.startswith(('torch=','adreno8_pause='))];args.append('torch=3')
 command=' '.join(args);assert len(command.encode())<416
 entries.update(kernel=kernel,ramdisk=ramdisk.read_bytes(),cmdline=command.encode()+b'\0')
 payload=api['pack'](entries);assert api['unpack'](payload)==entries
 (a.output/'payload.bin').write_bytes(payload)
 with a.system.open('rb') as stream:system_hash=hashlib.file_digest(stream,'sha256').hexdigest()
 meta.update(payload_sha256=sha(payload),kernel_sha256=sha(kernel),source_payload_sha256=sha(original),
  root_sha256=sha(entries['ramdisk']),system_sha256=system_hash,cmdline=command,cmdline_bytes=len(command),
  loader='experimental/uefi-turtle/screen-brightness-v13/dodge-ios-uefi.img',
  loader_sha256='5bafc800875bedf74c312910af396726451f493ed0f4e14d113ef0e1b8c651a0',
  phone_partition_writes=False,device_validated=False,screen_brightness_requested=True,screen_brightness_verified=False,
  control_center_brightness_verified=False,changed_entries=['kernel','ramdisk','cmdline'],root_files_verified=count)
 (a.output/'BUILD.json').write_text(json.dumps(meta,indent=2)+'\n')
 print('Verified RAM root and payload:',a.output/'payload.bin')
if __name__=='__main__':main()
