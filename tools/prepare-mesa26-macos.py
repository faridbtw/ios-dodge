#!/usr/bin/env python3
"""Prepare verified Mesa26.2.5 and audit the known patch drift before building.
Download mesa-26.2.5.tar.xz from archive.mesa3d.org to build/mesa26-ios first.
"""
from pathlib import Path
import subprocess,shutil,re,json,hashlib
r=Path(__file__).resolve().parents[1]; src=r/'build/mesa26-ios/mesa-26.2.5'; ref=r/'source/reference-k90-userland/tools/mesa'; out=r/'diagnostics/20261025-gen8-bringup/mesa26';out.mkdir(exist_ok=True)
assert not (src/'.op13-prepared').exists(), 'Source already prepared; use existing audited build'
import tarfile
archive=r/'build/mesa26-ios/mesa-26.2.5.tar.xz'
assert hashlib.sha256(archive.read_bytes()).hexdigest()=='794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5'
assert not src.exists(), 'Refuse to overwrite an existing source tree'
with tarfile.open(archive) as tar: tar.extractall(src.parent)
receipts=[]
for p in sorted(ref.glob('*.patch')):
 if p.name=='osmesa-es-profile.patch': continue
 s=p.read_text()
 if p.name=='freedreno-darwin.patch':
  parts=re.split(r'(?=^--- a/)',s,flags=re.M)
  s=''.join(x for x in parts if not re.match(r'--- a/src/gallium/(frontends|targets)/osmesa/',x))
 cmd=['patch','--batch','--forward','--no-backup-if-mismatch','-d',str(src),'-p1']
 v=subprocess.run(cmd,input=s,text=True,capture_output=True)
 (out/(p.stem+'.log')).write_text(v.stdout+v.stderr)
 receipts.append({'patch':str(p.relative_to(r)),'sha256':hashlib.sha256(s.encode()).hexdigest(),'exit':v.returncode})
 print(p.name,v.returncode)
for p in sorted((ref/'rebase-26').glob('*.patch')):
 v=subprocess.run(['patch','--batch','--forward','--no-backup-if-mismatch','-d',str(src),'-p1'],input=p.read_text(),text=True,capture_output=True)
 (out/(p.stem+'.log')).write_text(v.stdout+v.stderr);receipts.append({'patch':str(p.relative_to(r)),'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'exit':v.returncode});print(p.name,v.returncode)
for name,target in [('redwood-drm','src/redwood-drm'),('air','src/compiler/air')]: shutil.copytree(ref/name,src/target,dirs_exist_ok=True)
(out/'patch-receipts.json').write_text(json.dumps(receipts,indent=2)+'\n')
(src/'.op13-prepared').write_text('reference patch application; inspect rejection logs before build\n')

logs=out
resolutions={
'src/freedreno/drm/meson.build.rej':'Upstream now uses libfreedreno_drm_link_with, and adds libvdrm only inside virtio KMD condition.',
'src/gallium/drivers/freedreno/freedreno_screen.c.rej':'Upstream removed sysinfo and ram_size paths; no Darwin replacement needed.',
'src/gallium/drivers/freedreno/a6xx/fd6_draw.cc.rej':'The rebase-26/0003 patch supplies the framebuffer-read barrier at its new location.',
'src/gallium/drivers/freedreno/meson.build.rej':'Obsolete idep_air dependency for relocated offline ir3 command-line tool; tool not built in this runtime.',
'src/gallium/drivers/freedreno/ir3/ir3_cmdline.c.rej':'Obsolete relocated offline AIR compiler tool; runtime does not use it.',
'src/freedreno/ir3/ir3_nir_lower_io_offsets.c.rej':'Upstream removed iadd shift folding entirely; restore upstream file, which needs no added shift guard.'}
actual={str(p.relative_to(src)) for p in src.rglob('*.rej')};assert actual==set(resolutions)
assert "libfreedreno_drm_link_with += libvdrm" in (src/'src/freedreno/drm/meson.build').read_text()
assert '#include <sys/sysinfo.h>' not in (src/'src/gallium/drivers/freedreno/freedreno_screen.c').read_text()
assert 'prev_draw_read_fb' in (src/'src/gallium/drivers/freedreno/a6xx/fd6_draw.cc').read_text()
with tarfile.open(work/'mesa-26.2.5.tar.xz') as tar:
 name='src/freedreno/ir3/ir3_nir_lower_io_offsets.c'
 (src/name).write_bytes(tar.extractfile('mesa-26.2.5/'+name).read())
for name,why in resolutions.items():
 p=src/name; dest=logs/'resolved-rejections'/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.move(p,dest)
(logs/'resolved-rejections.json').write_text(json.dumps(resolutions,indent=2)+'\n')
