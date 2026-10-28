#!/usr/bin/env python3
"""Build a separate local Sileo system RAM image from the pinned iOS image."""
import argparse,hashlib,json,os,shutil,subprocess,sys
from pathlib import Path,PurePosixPath
ROOT=Path(__file__).resolve().parents[1]
APFS=ROOT/'source/hackios-xnu-userland/tools/apfs'
sys.path.insert(0,str(APFS));import apfs

def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
 p.add_argument('--system',type=Path,default=ROOT/'system.apfs')
 p.add_argument('--bootstrap',type=Path,default=ROOT/'source/hackios-xnu-userland/jb/root/var/jb')
 a=p.parse_args()
 original=a.system.resolve();assert sha(original)=='d147f97961429ff384a06960bf706b9bbab0d097b933e4b1bb1236c1f84cd432'
 bootstrap=a.bootstrap.resolve();assert (bootstrap/'Applications/Sileo.app/Sileo').is_file()
 out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);tree=out/'system-root';tree.mkdir()
 v=apfs.pick_volume(apfs.Container(str(original)),None);manifest=[];hashes={};count=0
 for path,_,oid,_ in apfs.walk(v,2):
  parts=PurePosixPath(path).parts[1:];assert '..' not in parts
  inode=v.inode(oid);dest=tree.joinpath(*parts)
  if inode.is_dir:dest.mkdir()
  elif inode.is_link:dest.symlink_to(os.fsdecode(inode.contents()))
  elif inode.is_reg:
   data=inode.contents();dest.write_bytes(data);hashes[path]=hashlib.sha256(data).hexdigest()
  else:raise ValueError('Unsupported inode '+path)
  if not inode.is_link:dest.chmod(inode.mode&0o7777)
  manifest.append(f'{inode.mode:o}\t{inode.owner}\t{inode.group}\t{path.lstrip("/")}');count+=1
  if count%10000==0:print('Extracted',count,'original entries',flush=True)
 shutil.copytree(bootstrap,tree/'jb',symlinks=True)
 apps=[]
 for app in sorted((tree/'jb/Applications').glob('*.app')):
  shutil.copytree(app,tree/'Applications'/app.name,symlinks=True);apps.append(app.name)
 for command in ['mv','cp','rm','mkdir','chmod']:
  dest=tree/'bin'/command;assert not dest.exists() and not dest.is_symlink();dest.symlink_to('/var/jb/usr/bin/'+command)
 manifest_path=out/'system.manifest';manifest_path.write_text('\n'.join(manifest)+'\n')
 (out/'original-files.json').write_text(json.dumps(hashes,sort_keys=True))
 print('Building separate Sileo RAM system; original system.apfs unchanged',flush=True)
 image=out/'system-sileo.apfs'
 subprocess.run([sys.executable,str(APFS/'mkapfs.py'),'-z','-o',str(image),f'System={tree},manifest={manifest_path}'],check=True)
 result=apfs.pick_volume(apfs.Container(str(image)),None);verified=0
 for path,digest in hashes.items():
  inode=result.inode(result.lookup(path)[0]);assert hashlib.sha256(inode.contents()).hexdigest()==digest,path;verified+=1
 for path,_,oid,_ in apfs.walk(result,2):
  if path.startswith('/jb/') or path.startswith('/Applications/Sileo.app/'):
   inode=result.inode(oid)
   if inode.is_reg:assert inode.contents()==(tree/path.lstrip('/')).read_bytes(),path
 receipt=dict(source_system_sha256=sha(original),system_sha256=sha(image),original_files_verified=verified,apps=apps,phone_partition_writes=False,ram_only=True)
 (out/'SYSTEM.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt),flush=True)
if __name__=='__main__':main()
