#!/usr/bin/env python3
"""Validate and RAM-boot the isolated GLES/battery/slider integration on macOS."""
import argparse
import fcntl
import hashlib
import json
import re
from pathlib import Path
import runpy
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--payload', required=True, type=Path)
    p.add_argument('--serial')
    p.add_argument('--output', type=Path)
    p.add_argument('--check', action='store_true')
    p.add_argument('--loader', type=Path, help='Use the pinned v12 live cold-reset UEFI')
    a = p.parse_args()
    payload = a.payload.resolve()
    meta = json.loads((payload.parent / 'BUILD.json').read_text())
    assert not meta['phone_partition_writes']
    assert digest(payload) == meta['payload_sha256']
    entries = runpy.run_path(str(ROOT / 'tools/make-op13-test-payload.py'))['unpack'](payload.read_bytes())
    assert hashlib.sha256(entries['kernel']).hexdigest() == meta['kernel_sha256']
    assert entries['cmdline'].rstrip(b'\0').decode() == meta['cmdline']
    args = dict(t.split('=', 1) for t in meta['cmdline'].split())
    required = dict(cpus='7', bootcpu='1', op13_usb='1', op13_batt='4', slider='1', slider_power='1',
                    gl='1', metal='0', adreno8='23', adreno8_keep='1', adreno8_secure_preflight='4',
                    ufs='0', ufs_write='0', var_store='0', cpufreq='0', loader_return_after='0',
                    touch_steps='4', buttons_steps='5')
    assert all(args.get(k) == v for k, v in required.items())
    assert args.get('adreno8_return', '0') == '0'
    assert 'op13_audio' not in args and 'sm8750_test_reset_seconds' not in args
    assert len(entries['cmdline']) <= 416
    loader = ROOT / meta['loader']
    pinned_loaders = {
        ROOT / 'experimental/uefi-turtle/stable-overlay-v11/dodge-ios-uefi.img':
            '36e6d04439b68a425e94f14a8c7d0648fd89f3b2675609685e03999160226934',
        ROOT / 'experimental/uefi-turtle/live-reset-v12/dodge-ios-uefi.img':
            '40861c9e4258b30902fc806caad83a09d104001e3587434aabeb046d466c913b',
    }
    assert loader in pinned_loaders and digest(loader) == meta['loader_sha256'] == pinned_loaders[loader]
    if a.loader:
        loader = a.loader.resolve()
        assert loader == ROOT / 'experimental/uefi-turtle/live-reset-v12/dodge-ios-uefi.img'
        assert digest(loader) == pinned_loaders[loader]
    if loader == ROOT / 'experimental/uefi-turtle/live-reset-v12/dodge-ios-uefi.img':
        final = json.loads(loader.with_suffix('.json').read_text())
        base = ROOT / final['source']
        first = json.loads(base.with_suffix('.json').read_text())
        assert digest(base) == final['source_sha256'] == first['image_sha256']
        assert first['source_sha256'] == 'f26410ca510dcb7352f5c0b814ca435aaefe05bdc3ed1697ef5091b79d588cb8'
        assert final['changed_inner_ffs'] == ['7bb28b99-61bb-11d5-9a5d-0090273fc14d']
        assert set(first['changed_inner_ffs']) == {'fb14ecd8-9432-48b8-a692-ae4bc19d568b',
            '50670071-478f-4be7-ad13-8754f379c62f', '643d4788-63c8-40d2-bd7a-6842e66850d1'}
        assert all(first[k] and final[k] for k in ('bootshim_unchanged', 'dtb_unchanged',
                                                  'ramdisk_and_signature_bytes_unchanged'))
        assert first['other_ffs_unchanged'] and final['other_inner_ffs_unchanged']
    system = ROOT / 'system.apfs'
    assert digest(system) == meta['system_sha256']
    if a.check:
        print(json.dumps(dict(package_verified=True, system_verified=True, device_contacted=False,
                             cmdline_bytes=meta['cmdline_bytes']), indent=2))
        return
    if not a.serial or not a.output:
        p.error('--serial and --output are required to boot')
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    receipts = []

    def run(argv, timeout=15):
        argv = list(map(str, argv))
        try:
            r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
            receipts.append(dict(args=argv, code=r.returncode, output=r.stdout + r.stderr))
            (out / 'receipts.json').write_text(json.dumps(receipts, indent=2) + '\n')
            r.check_returncode()
            return r.stdout + r.stderr
        except subprocess.TimeoutExpired:
            receipts.append(dict(args=argv, timeout=timeout))
            (out / 'receipts.json').write_text(json.dumps(receipts, indent=2) + '\n')
            raise

    def fb(words, serial='xnuloader', timeout=15):
        assert words[0] in ('devices', 'getvar', 'get_staged', 'boot', 'stage', 'oem')
        if words[0] == 'oem':
            assert words[1:] in (['ramdisk'], ['op13-slider-probe'], ['sm8750-touch-arm'], ['run', '0'], ['log'])
        return run(['fastboot', '-s', serial, *words], timeout)

    def wait_device(serial):
        for _ in range(45):
            listed = run(['fastboot', 'devices'], 5)
            if any(line.split() and line.split()[0] == serial for line in listed.splitlines()):
                return
            time.sleep(1)
        raise RuntimeError('USB enumeration timed out')

    with (ROOT / 'build/gles-ram-boot.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        assert run(['adb', '-s', a.serial, 'get-state']).strip() == 'device'
        assert run(['adb', '-s', a.serial, 'shell', 'getprop', 'ro.board.platform']).strip() == 'sun'
        assert run(['adb', '-s', a.serial, 'shell', 'getprop', 'sys.boot_completed']).strip() == '1'
        print('Android verified; booting turtle UEFI into RAM', flush=True)
        run(['adb', '-s', a.serial, 'reboot', 'bootloader'])
        wait_device(a.serial)
        assert 'product: sun' in fb(['getvar', 'product'], a.serial)
        fb(['boot', loader], a.serial, 50)
        wait_device('xnuloader')
        assert 'product: xnuloader' in fb(['getvar', 'product'])
        fb(['stage', payload], timeout=60)
        fb(['oem', 'ramdisk'])
        print('Uploading system.apfs into RAM', flush=True)
        fb(['stage', system], timeout=180)
        fb(['oem', 'op13-slider-probe'])
        fb(['oem', 'sm8750-touch-arm'], timeout=30)
        fb(['oem', 'run', '0'], timeout=30)
        result = dict(ram_boot_started=True, phone_partition_writes=False, automatic_return_seconds=0,
                      hardware_acceleration_verified=False, payload_sha256=meta['payload_sha256'],
                      loader=str(loader.relative_to(ROOT)), loader_sha256=digest(loader))
        (out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
        print('GLES iOS started; collecting live logs', flush=True)
        for index in range(3):
            time.sleep(20)
            result['loader_result'] = fb(['getvar', 'result'])
            fb(['oem', 'log'])
            path = out / f'runtime-{index}.log'
            fb(['get_staged', path], timeout=30)
            text = path.read_bytes().replace(b'\0', b'').decode('utf-8', 'replace')
            result['hardware_acceleration_verified'] = 'OpenGL ES 3.2 Mesa 26.2.5; Adreno (TM) 830' in text
            result['battery_worker_started'] = 'op13-battery: phase 4 worker started' in text
            result['slider_sensor_ready'] = 'op13-slider: AK09970 ready' in text
            result['panic_detected'] = 'panic(cpu' in text
            if meta.get('display_commit_fix_requested'):
                samples = [tuple(map(int, row)) for row in re.findall(
                    r'op13-display: commit (\d+) TE=(\d+) period-us=(\d+) wait-us=(\d+) copy-us=(\d+) failures=(\d+)', text)]
                result['display_shadow_enabled'] = result.get('display_shadow_enabled', False) or 'complete-frame shadow composition enabled' in text
                if samples:
                    result['display_te_period_us'] = samples[-1][2]
                    result['display_te_sync_observed'] = any(row[1] for row in samples)
                    result['display_max_copy_us'] = max(result.get('display_max_copy_us', 0), max(row[4] for row in samples))
                    result['display_commit_fits_refresh'] = all(row[1] and 0 < row[4] < row[2] for row in samples)
            if meta.get('coregraphics_preview_requested'):
                result['preview_hook_installed'] = result.get('preview_hook_installed', False) or 'CoreGraphics preview installed' in text
                result['preview_capture_ready_count'] = max(result.get('preview_capture_ready_count', 0), sum(
                    'op13-wallpaper: CoreGraphics preview ' in line and 'image=ready' in line
                    for line in text.splitlines()))
                result['posterboard_abort_records'] = max(result.get('posterboard_abort_records', 0), sum(
                    'com.apple.PosterBoard [' in line and 'exited due to SIGABRT' in line
                    for line in text.splitlines()))
                result['catalog_timeout_count'] = max(result.get('catalog_timeout_count', 0), text.count('proactive timed out'))
            (out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
            print(json.dumps(result), flush=True)


if __name__ == '__main__':
    main()
