#!/usr/bin/env python3
"""Run bounded RAM-only QMI steps after successful MHI startup; save each log."""
import argparse
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--steps', nargs='+', type=int, choices=[2, 1, 3, 0, 4], default=[2, 1, 3, 0])
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
receipts = []


def run(args, timeout):
    try:
        r = subprocess.run(['fastboot', '-s', 'xnuloader', *args],
                           capture_output=True, text=True, timeout=timeout)
        receipts.append({'command': args, 'exit': r.returncode,
                         'output': r.stdout + r.stderr})
        print(r.stdout + r.stderr, flush=True)
        return r.returncode
    except subprocess.TimeoutExpired:
        receipts.append({'command': args, 'timeout': timeout})
        raise SystemExit('Timed out; inspect recovery state. Do not repeat endpoint access.')
    finally:
        (a.output / 'receipts.json').write_text(json.dumps(receipts, indent=2) + '\n')


for step in a.steps:
    status = run(['oem', 'op13-wifi-smmu', str(step)], 60 if step == 0 else 28)
    if run(['oem', 'log'], 5) or run(['get_staged', str(a.output / f'step-{step}.log')], 5):
        raise SystemExit('Log capture failed; inspect recovery state.')
    if status:
        raise SystemExit(status)
