import os
from pathlib import Path
import json,tarfile,shutil
out=Path(os.environ['IOS19_RAM_DIR']).expanduser().resolve()
rows=json.loads((out/'archive-inventory.json').read_text())
prefix='System/Library/PrivateFrameworks/IDS.framework/identityservicesd.app/'
cryp='System/Cryptexes/OS/System/Library/PrivateFrameworks/'
frameworks=['CloudAssetsDaemon.framework','PlugInKitDaemon.framework']
wanted={r['path']:r for r in rows if r['type']=='0' and (r['path'].startswith(prefix) or any(r['path'].startswith(cryp+f+'/') for f in frameworks))}
root=out/'boot-root/native-support';seen=set()
with tarfile.open(out.parent/'D84AP_23A191a_RootFS.tgz','r|gz') as arc:
 for m in arc:
  name=m.name.removeprefix('./')
  if name not in wanted:continue
  p=root/name.removeprefix('System/Cryptexes/OS/');p.parent.mkdir(parents=True,exist_ok=True)
  with arc.extractfile(m) as src,p.open('wb') as dest:shutil.copyfileobj(src,dest)
  p.chmod(wanted[name]['mode']);seen.add(name)
  if seen==set(wanted):break
assert seen==set(wanted)
# Keep existing framework resources alongside restored executable overlays.
for f in ['IDS.framework',*frameworks]:
 src=out/'system-root/System/Library/PrivateFrameworks'/f
 if src.exists():shutil.copytree(src,root/'System/Library/PrivateFrameworks'/f,dirs_exist_ok=True,symlinks=True)
# Native launchd definition, on demand, logging to the existing console.
import plistlib
job=plistlib.loads((out/'system-root/System/Library/LaunchDaemons/com.apple.identityservicesd.plist').read_bytes())
job.pop('RunAtLoad',None);job.pop('KeepAlive',None)
job['StandardOutPath']='/dev/console';job['StandardErrorPath']='/dev/console'
p=root/'System/Library/LaunchDaemons/com.apple.identityservicesd.plist';p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(plistlib.dumps(job))
(out/'NATIVE-SERVICES.json').write_text(json.dumps({'original_files':sorted(seen),'bytes':sum(x['size'] for x in wanted.values()),'method':'original archive files, PID1 union overlays, native on-demand identityservicesd job'},indent=2)+'\n')
print('Restored',len(seen),'original files.',flush=True)
