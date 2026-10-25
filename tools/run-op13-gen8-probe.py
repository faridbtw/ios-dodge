#!/usr/bin/env python3
"""Stage a bounded Gen8 probe into an already running xnuloader, using RAM only."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import runpy
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--payload', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=60)
    args = parser.parse_args()
    payload = args.payload.resolve()
    unpack = runpy.run_path(str(ROOT / 'tools/make-op13-test-payload.py'))['unpack']
    entries = unpack(payload.read_bytes())
    tokens = entries['cmdline'].rstrip(b'\0').decode().split()
    boot = dict(token.split('=', 1) for token in tokens)
    for key in ['gpu', 'adreno', 'metal', 'ufs', 'ufs_write', 'var_store', 'cpufreq']:
        if boot.get(key) != '0': raise SystemExit(f'Unexpected {key}: refusing probe')
    stage = int(boot.get('adreno8', '0'))
    if not 1 <= stage <= 13 or boot.get('adreno8_return') != '1' or boot.get('adreno8_keep') != '0':
        raise SystemExit('Expected a returning, non-retained stage 1..13 probe')
    if boot.get('sm8750_gpu') != '1' or boot.get('gicr_stride') != '0x40000':
        raise SystemExit('Expected explicit SM8750 guard and redistributor stride')
    if b'adreno8: Dodge currently' not in entries['kernel']:
        raise SystemExit('Expected separately gated Dodge Gen8 kernel')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    receipts = []

    def call(command, timeout=8):
        # This closed command set cannot flash, erase, reboot or replace the resident root.
        permitted = (command[:1] in [['getvar'], ['get_staged']] or
            command == ['stage', str(payload)] or command in
            [['oem', 'sm8750-touch-arm'], ['oem', 'run', '0'], ['oem', 'log']])
        if not permitted: raise ValueError('Unexpected fastboot operation')
        started = time.monotonic()
        try:
            result = subprocess.run(['fastboot', '-s', 'xnuloader', *command],
                capture_output=True, text=True, timeout=timeout)
            receipts.append({'command': command, 'exit': result.returncode,
                'elapsed': time.monotonic() - started, 'output': result.stdout + result.stderr})
            if result.returncode: raise RuntimeError(result.stdout + result.stderr)
            return result.stdout + result.stderr
        finally:
            (out / 'receipts.json').write_text(json.dumps(receipts, indent=2) + '\n')

    if 'xnuloader' not in call(['getvar', 'product']): raise SystemExit('Wrong loader')
    if '0x03f00000' not in call(['getvar', 'max-download-size']):
        raise SystemExit('Resident-root transfer is armed; refusing to overwrite system RAM')
    call(['stage', str(payload)], timeout=15)
    call(['oem', 'sm8750-touch-arm'], timeout=15)
    call(['oem', 'run', '0'])
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        time.sleep(0.5)
        try:
            result = call(['getvar', 'result'], timeout=2)
        except subprocess.TimeoutExpired:
            continue
        if 'returned ' in result: break
    else:
        raise SystemExit('Probe did not return within the deadline; no further staging attempted')
    call(['oem', 'log'])
    call(['get_staged', str(out / 'kernel.log')])
    log = (out / 'kernel.log').read_text(errors='replace')
    completed = sorted(set(map(int, re.findall(r'adreno8: (\d+): completed', log))))
    failed = sorted(set(map(int, re.findall(r'adreno8: (\d+): FAILED', log))))
    panic = 'panic(cpu' in log
    result = {'requested_stage': stage, 'completed_stages': completed,
        'failed_stages': failed, 'panic': panic, 'hardware_rendering_proven': False,
        'payload_sha256': hashlib.sha256(payload.read_bytes()).hexdigest(),
        'kernel_sha256': hashlib.sha256(entries['kernel']).hexdigest(),
        'phone_partition_writes': False}
    (out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 0 if stage in completed and not failed and not panic else 1


if __name__ == '__main__':
    raise SystemExit(main())
