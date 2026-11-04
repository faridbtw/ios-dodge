import os
from pathlib import Path
import json,tarfile,shutil,time
out=Path(os.environ['IOS19_RAM_DIR']).expanduser().resolve();rows=json.loads((out/'archive-inventory.json').read_text());wanted={r['path']:r for r in rows if r['path'].startswith('System/Cryptexes/OS/usr/') and r['type']=='0'}
assert len(wanted)==10
root=out/'boot-root/cryptex';root.mkdir(parents=True,exist_ok=True);seen=set()
with tarfile.open(out.parent/'D84AP_23A191a_RootFS.tgz','r|gz') as arc:
 for m in arc:
  name=m.name.removeprefix('./')
  if name in wanted:
   p=root/name[len('System/Cryptexes/OS/'):];p.parent.mkdir(parents=True,exist_ok=True)
   with arc.extractfile(m) as source,p.open('wb') as target:shutil.copyfileobj(source,target)
   p.chmod(wanted[name]['mode']);seen.add(name);print('Restored cryptex support '+p.name,flush=True)
   if seen==set(wanted):break
assert seen==set(wanted)
(out/'boot-root/etc/rc').write_text('exec /redwood/bin/cryptex-links\n')
(out/'CRYPTEX-SUPPORT.json').write_text(json.dumps({'files_restored':sorted(wanted),'source':'original archive/System/Cryptexes/OS/usr','method':'PID1 read-only union bindfs directory overlays before native launchd; no substitute implementations','original_system_apfs_unchanged':True},indent=2)+'\n')
print('Original support files restored. Build tools/cryptex-links.c as the static PID1 handoff helper.',flush=True)
