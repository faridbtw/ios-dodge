import os
from pathlib import Path
import struct,json,hashlib,subprocess,zlib,importlib.util
out=Path(os.environ['IOS19_RAM_DIR']).expanduser().resolve();project=out.parents[1]/'OnePlus13-iOS-RAM';plan=json.loads((out/'SWIFT-PATCH-PLAN.json').read_text());name=plan['image'];relative='System/Cryptexes/OS/System/Library/Caches/com.apple.dyld/'+name
file=out/'boot-system-root'/relative;old=file.read_bytes();new=bytearray(old);changes=plan['changes'];assert len(changes)==8
for r in changes:
 offset=plan['mapping_fileoff']+int(r['va'],16)-int(plan['mapping_va'],16);assert struct.unpack_from('<I',new,offset)[0]==int(r['before'],16);struct.pack_into('<I',new,offset,int(r['after'],16))
sig,size=struct.unpack_from('<QQ',new,40);magic,length,count=struct.unpack_from('>III',new,sig);assert magic==0xfade0cc0 and sig+size==len(new);hashchanges=[]
for i in range(count):
 slot,off=struct.unpack_from('>II',new,sig+12+8*i);at=sig+off;magic,length=struct.unpack_from('>II',new,at)
 if slot==0x10000:assert magic==0xfade0b01 and length==8
 if magic!=0xfade0c02:continue
 version,flags,hoff,ident,special,slots,limit=struct.unpack_from('>7I',new,at+8);hs,ht,platform,shift=struct.unpack_from('4B',new,at+36)
 assert version==0x20400 and flags&2 and hs==32 and ht==2 and limit==sig
 for page in range(slots):
  digest=hashlib.sha256(new[page*(1<<shift):min((page+1)*(1<<shift),limit)]).digest();pos=at+hoff+page*hs
  if new[pos:pos+hs]!=digest:new[pos:pos+hs]=digest;hashchanges.append(page)
 for page in range(slots):assert new[at+hoff+page*hs:at+hoff+(page+1)*hs]==hashlib.sha256(new[page*(1<<shift):min((page+1)*(1<<shift),limit)]).digest()
image=out/'boot-system.apfs';backup=out/'boot-system-before-swift.apfs'
if not backup.exists():subprocess.run(['/bin/cp','-c',str(image),str(backup)],check=True)
spec=importlib.util.spec_from_file_location('apfs_reader',project/'experimental/ios27/v2/userland-work/tools/apfs/apfs.py');a=importlib.util.module_from_spec(spec);spec.loader.exec_module(a);c=a.Container(str(image));v=c.volumes()[0];ino=v.inode(v.lookup('/'+relative)[0]);assert ino.contents()==old
flags,data=ino.xattrs[a.RSRC_FORK_XATTR_NAME];assert flags&a.XATTR_DATA_STREAM;sid,streamsize=struct.unpack_from('<QQ',data);extents=v.extents(sid);assert len(extents)==1 and extents[0][0]==0 and extents[0][1]>=streamsize;rsrc=v.read_stream(sid,streamsize);do,mo,dl,ml=struct.unpack_from('>IIII',rsrc);base=do+4;count=struct.unpack_from('<I',rsrc,base)[0];parts=[];changed=[]
for i in range(count):
 offset,length=struct.unpack_from('<II',rsrc,base+4+8*i);part=rsrc[base+offset:base+offset+length];chunk=bytes(new[i*65536:(i+1)*65536])
 if chunk!=old[i*65536:(i+1)*65536]:
  choices=[b'\xff'+chunk]
  for mem in (8,9):
   for strategy in (zlib.Z_DEFAULT_STRATEGY,zlib.Z_FILTERED,zlib.Z_FIXED):
    z=zlib.compressobj(9,zlib.DEFLATED,15,mem,strategy);choices.append(z.compress(chunk)+z.flush())
  part=min(choices,key=len);changed.append(i)
 parts.append(part)
body=bytearray(struct.pack('<I',count));offset=4+8*count
for part in parts:body+=struct.pack('<II',offset,len(part));offset+=len(part)
body+=b''.join(parts);nd=struct.pack('>I',len(body))+body;nm=do+len(nd);rmap=bytearray(rsrc[mo:mo+ml]);header=struct.pack('>IIII',do,nm,len(nd),ml);rmap[:16]=header;replacement=header+bytes(do-16)+nd+rmap
assert len(replacement)<=streamsize,(len(replacement),streamsize);assert a.decmpfs_rsrc(4,replacement,len(new))==new
physical=extents[0][2]*c.block_size;c.close()
with image.open('r+b') as f:f.seek(physical);f.write(replacement);f.write(bytes(streamsize-len(replacement)))
c=a.Container(str(image));v=c.volumes()[0];assert v.inode(v.lookup('/'+relative)[0]).contents()==new;c.close()
tmp=file.with_suffix('.swift-mask-tmp');tmp.write_bytes(new);tmp.chmod(file.stat().st_mode);tmp.replace(file)
source=out/'system-root'/relative;assert hashlib.sha256(source.read_bytes()).digest()==hashlib.sha256(old).digest()
receipt={'status':'patched-and-readback-verified','scope':'eight Swift class data pointer masks39->38bits; selected by fatal PC and Swift __text census','changes':changes,'changed_code_pages':hashchanges,'code_signature':'existing ad-hoc SHA256 CodeDirectory refreshed; all pages verified','changed_compression_chunks':changed,'old_resource_bytes':streamsize,'new_resource_bytes':len(replacement),'apfs_metadata_unchanged':True,'boot_image_backup':str(backup),'stock_selected_system_source_unchanged':True,'old_cache_sha256':hashlib.sha256(old).hexdigest(),'new_cache_sha256':hashlib.sha256(new).hexdigest()}
(out/'SWIFT-MASK-PATCH.json').write_text(json.dumps(receipt,indent=2)+'\n');print('Swift mask patch verified:8instructions, '+str(len(hashchanges))+' code pages; APFS resource stream fits and independently reads back',flush=True)
profile=json.loads((out/'BOOT-PREFLIGHT.json').read_text());profile['images']['system']['bytes']=image.stat().st_size
with image.open('rb') as f:profile['images']['system']['sha256']=hashlib.file_digest(f,'sha256').hexdigest()
profile['swift_class_mask_patch']=True;(out/'BOOT-PREFLIGHT.json').write_text(json.dumps(profile,indent=2)+'\n')
