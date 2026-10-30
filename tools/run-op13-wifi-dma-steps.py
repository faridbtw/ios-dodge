#!/usr/bin/env python3
"""Capture each bounded Wi-Fi diagnostic step, including failure evidence."""
import argparse,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',required=True,type=Path)
p.add_argument('--steps',nargs='+',type=int,choices=[9,8,7,6],default=[9,8,7])
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);receipts=[]
def run(args,timeout):
    try:
        r=subprocess.run(['fastboot','-s','xnuloader',*args],capture_output=True,text=True,timeout=timeout)
        receipts.append({'command':args,'exit':r.returncode,'output':r.stdout+r.stderr})
        print(r.stdout+r.stderr,flush=True)
        return r.returncode
    except subprocess.TimeoutExpired as e:
        receipts.append({'command':args,'timeout':timeout,'output':((e.stdout or b'')+(e.stderr or b'')).decode(errors='replace')})
        return 124
    finally:
        (a.output/'receipts.json').write_text(json.dumps(receipts,indent=2)+'\n')
for step in a.steps:
    result=run(['oem','op13-wifi-smmu',str(step)],28)
    if result==124:raise SystemExit('Timed out; resources retained, inspect recovery state')
    if run(['oem','log'],5) or run(['get_staged',str(a.output/f'step-{step}.log')],5):
        raise SystemExit('Log capture failed; inspect recovery state')
    for line in (a.output/f'step-{step}.log').read_text(errors='replace').splitlines():
        if line.startswith(('op13-wifi-dma:','op13-wifi-bhi:')):print(line,flush=True)
    if result:raise SystemExit(result)
