import os
from pathlib import Path
import json,tarfile,shutil
out=Path(os.environ['IOS19_RAM_DIR']).expanduser().resolve();prefix='System/Cryptexes/OS/System/Library/PrivateFrameworks/'
rows=json.loads((out/'archive-inventory.json').read_text())
wanted={r['path']:r for r in rows if r['type']=='0' and r['path'].startswith(prefix) and len(r['path'][len(prefix):].split('/'))==2 and Path(r['path']).suffix==''}
root=out/'boot-root/native-support/System/Library/PrivateFrameworks';seen=set()
for r in wanted.values():
 framework=r['path'][len(prefix):].split('/')[0];src=out/'system-root/System/Library/PrivateFrameworks'/framework
 if src.exists():shutil.copytree(src,root/framework,dirs_exist_ok=True,symlinks=True)
with tarfile.open(out.parent/'D84AP_23A191a_RootFS.tgz','r|gz') as arc:
 for m in arc:
  name=m.name.removeprefix('./')
  if name not in wanted:continue
  p=root/name[len(prefix):];p.parent.mkdir(parents=True,exist_ok=True)
  with arc.extractfile(m) as src,p.open('wb') as dst:shutil.copyfileobj(src,dst)
  p.chmod(m.mode);seen.add(name)
  if seen==set(wanted):break
assert seen==set(wanted)
(out/'CRYPTEX-FRAMEWORKS.json').write_text(json.dumps({'original_executables':sorted(wanted),'bytes':sum(x['size'] for x in wanted.values()),'method':'original loose Cryptex framework executables with original resource directories in RAM union overlay'},indent=2)+'\n')
print('Restored',len(seen),'original framework executables.',flush=True)
