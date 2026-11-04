import os
from pathlib import Path
import tarfile,shutil,hashlib,json
out=Path(os.environ['IOS19_RAM_DIR']).expanduser().resolve()
rel='System/Library/PrivateFrameworks/UIKitCore.framework/Artwork.bundle'
dest=out/'boot-root/ui-artwork';shutil.copytree(out/'system-root'/rel,dest,dirs_exist_ok=True,symlinks=True)
wanted=rel+'/Assets.car'
with tarfile.open(out.parent/'D84AP_23A191a_RootFS.tgz','r|gz') as arc:
 for m in arc:
  if m.name.removeprefix('./')==wanted:
   with arc.extractfile(m) as a,(dest/'Assets.car').open('wb') as b:shutil.copyfileobj(a,b)
   (dest/'Assets.car').chmod(m.mode);break
 else:raise RuntimeError('Original catalog absent')
p=dest/'Assets.car';assert p.stat().st_size==6812816
(out/'UI-ARTWORK.json').write_text(json.dumps({'source':wanted,'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'method':'original complete Artwork.bundle files with restored catalog, RAM bind overlay'},indent=2)+'\n')
print('Restored original UIKit artwork catalog',p.stat().st_size,flush=True)
