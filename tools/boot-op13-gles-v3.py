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
    p.add_argument('--system', type=Path, help='Explicit local system RAM image matching BUILD.json')
    p.add_argument('--output', type=Path)
    p.add_argument('--check', action='store_true')
    p.add_argument('--menu', action='store_true', help='Upload iOS into RAM, then wait for the phone Boot iOS button')
    p.add_argument('--loader', type=Path, help='Use a pinned iOS UEFI')
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
    project_boot_loader = ROOT / 'experimental/uefi-menu/project-boot-v24/dodge-ios-uefi.img'
    pinned_loaders = {
        project_boot_loader: '2039c1e51d8a35cf30c771c4985ea30c4ee8103bc9ed0dcbaf0a9d58b5eee6ab',
        ROOT / 'experimental/uefi-menu/project-boot-v23/dodge-ios-uefi.img': 'f4cf919c432155494b9651006118b0a38bb8a3749f2c8ea43527ccba28ed385f',
        ROOT / 'experimental/uefi-menu/project-boot-v22/dodge-ios-uefi.img': '5dfb3be4b081c8a5a94ea3f4be5fed5afdc4212fb01fbf24632096691cca1584',
        ROOT / 'experimental/uefi-menu/project-boot-v21/dodge-ios-uefi.img': '50d94ffe071f15071ce683b9d95a592a2d45dbdb56d4f4bc66ef2ff6e59e27d6',
        ROOT / 'experimental/uefi-menu/project-boot-v20/dodge-ios-uefi.img': '44161a987a8a7576f5ad4ad74e1ec1f4206d0d9c8eed6c98ccd6e963b75c0a33',
        ROOT / 'experimental/uefi-menu/project-boot-v19/dodge-ios-uefi.img': '49130b3bd1326eb2b87beec4674370c0658deaf50ae92c5460db04820f6cfc12',
        ROOT / 'experimental/uefi-menu/project-boot-v18/dodge-ios-uefi.img': '68a8050e97e55abb938dc3569c48ab12c4619fca2f11c81b768b7d223e692626',
        ROOT / 'experimental/uefi-menu/project-boot-v17/dodge-ios-uefi.img': '7a8d69b687e6423080b355869bc6b6519480d7e7d59571011ad34f0f0cddba50',
        ROOT / 'experimental/uefi-menu/project-boot-v16/dodge-ios-uefi.img': '7c04cb6e028cf03d2743975d11b03f5de096d01f63b54c553f202eaaa714b814',
        ROOT / 'experimental/uefi-menu/ios-v15/dodge-ios-uefi.img':
            'e3eae832a70b71b44e3bc17d610e568ce7e331b03bb56ff3e18678d7beaf0f93',
        ROOT / 'experimental/uefi-menu/android-v14/dodge-ios-uefi.img':
            'b713315db1b2fc027ef9a92a2de53f81fb9741b0aaa3ab093a9a1872c54ecc2c',
        ROOT / 'experimental/uefi-turtle/stable-overlay-v11/dodge-ios-uefi.img':
            '36e6d04439b68a425e94f14a8c7d0648fd89f3b2675609685e03999160226934',
        ROOT / 'experimental/uefi-turtle/live-reset-v12/dodge-ios-uefi.img':
            '40861c9e4258b30902fc806caad83a09d104001e3587434aabeb046d466c913b',
        ROOT / 'experimental/uefi-turtle/screen-brightness-v13/dodge-ios-uefi.img':
            '5bafc800875bedf74c312910af396726451f493ed0f4e14d113ef0e1b8c651a0',
    }
    assert loader in pinned_loaders and digest(loader) == meta['loader_sha256'] == pinned_loaders[loader]
    if a.loader:
        loader = a.loader.resolve()
        assert loader in pinned_loaders
        assert digest(loader) == pinned_loaders[loader]
    screen_bridge = meta.get('screen_brightness_requested', False)
    if screen_bridge:
        assert loader in (project_boot_loader, ROOT / 'experimental/uefi-menu/project-boot-v23/dodge-ios-uefi.img', ROOT / 'experimental/uefi-menu/project-boot-v22/dodge-ios-uefi.img', ROOT / 'experimental/uefi-menu/project-boot-v21/dodge-ios-uefi.img', ROOT / 'experimental/uefi-menu/project-boot-v20/dodge-ios-uefi.img', ROOT / 'experimental/uefi-menu/project-boot-v19/dodge-ios-uefi.img', ROOT / 'experimental/uefi-menu/project-boot-v18/dodge-ios-uefi.img', ROOT / 'experimental/uefi-menu/project-boot-v17/dodge-ios-uefi.img', ROOT / 'experimental/uefi-menu/project-boot-v16/dodge-ios-uefi.img', ROOT / 'experimental/uefi-turtle/screen-brightness-v13/dodge-ios-uefi.img',
                          ROOT / 'experimental/uefi-menu/android-v14/dodge-ios-uefi.img',
                          ROOT / 'experimental/uefi-menu/ios-v15/dodge-ios-uefi.img')
    if loader in (project_boot_loader, ROOT / 'experimental/uefi-menu/project-boot-v23/dodge-ios-uefi.img', ROOT / 'experimental/uefi-menu/project-boot-v22/dodge-ios-uefi.img', ROOT / 'experimental/uefi-menu/project-boot-v21/dodge-ios-uefi.img', ROOT / 'experimental/uefi-menu/project-boot-v20/dodge-ios-uefi.img', ROOT / 'experimental/uefi-menu/project-boot-v19/dodge-ios-uefi.img', ROOT / 'experimental/uefi-menu/project-boot-v18/dodge-ios-uefi.img', ROOT / 'experimental/uefi-menu/project-boot-v17/dodge-ios-uefi.img', ROOT / 'experimental/uefi-menu/project-boot-v16/dodge-ios-uefi.img', ROOT / 'experimental/uefi-menu/ios-v15/dodge-ios-uefi.img',
                  ROOT / 'experimental/uefi-menu/android-v14/dodge-ios-uefi.img',
                  ROOT / 'experimental/uefi-turtle/live-reset-v12/dodge-ios-uefi.img',
                  ROOT / 'experimental/uefi-turtle/screen-brightness-v13/dodge-ios-uefi.img'):
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
    if a.menu:
        assert loader in (project_boot_loader, ROOT / 'experimental/uefi-menu/project-boot-v23/dodge-ios-uefi.img', ROOT / 'experimental/uefi-menu/project-boot-v22/dodge-ios-uefi.img', ROOT / 'experimental/uefi-menu/project-boot-v21/dodge-ios-uefi.img', ROOT / 'experimental/uefi-menu/project-boot-v20/dodge-ios-uefi.img', ROOT / 'experimental/uefi-menu/project-boot-v19/dodge-ios-uefi.img', ROOT / 'experimental/uefi-menu/project-boot-v18/dodge-ios-uefi.img', ROOT / 'experimental/uefi-menu/project-boot-v17/dodge-ios-uefi.img', ROOT / 'experimental/uefi-menu/project-boot-v16/dodge-ios-uefi.img', ROOT / 'experimental/uefi-menu/ios-v15/dodge-ios-uefi.img'), '--menu requires the iOS button UEFI'
    system = a.system.resolve() if a.system else ROOT / 'system.apfs'
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
            assert words[1:] in (['ramdisk'], ['op13-slider-probe'], ['sm8750-touch-arm'], ['run', '0'], ['log'], ['menu-ready']) or (
                screen_bridge and words[1:] == ['op13-dsi-brightness', 'ahb'])
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
        print('Android verified; booting iOS UEFI into RAM', flush=True)
        run(['adb', '-s', a.serial, 'reboot', 'bootloader'])
        wait_device(a.serial)
        assert 'product: sun' in fb(['getvar', 'product'], a.serial)
        fb(['boot', loader], a.serial, 50)
        wait_device('xnuloader')
        assert 'product: xnuloader' in fb(['getvar', 'product'])
        if screen_bridge:
            fb(['oem', 'op13-dsi-brightness', 'ahb'])
            fb(['oem', 'log'])
            fb(['get_staged', out / 'screen-preflight.log'])
            preflight = (out / 'screen-preflight.log').read_bytes().replace(b'\0', b'').decode(errors='replace')
            assert 'config-AHB vote=0 on-status=0 on=1' in preflight
            assert 'version=20090000 CTRL=1f5' in preflight
        fb(['stage', payload], timeout=60)
        fb(['oem', 'ramdisk'])
        print('Uploading ' + system.name + ' into RAM', flush=True)
        fb(['stage', system], timeout=180)
        fb(['oem', 'op13-slider-probe'])
        if a.menu:
            fb(['oem', 'menu-ready'], timeout=30)
            (out / 'RESULT.json').write_text(json.dumps(dict(
                ram_boot_started=False, awaiting_phone_boot_button=True,
                phone_partition_writes=False, loader=str(loader.relative_to(ROOT)),
                loader_sha256=digest(loader), payload_sha256=meta['payload_sha256'],
                system=str(system), system_sha256=digest(system)), indent=2) + '\n')
            print('iOS ready in RAM. Tap Boot iOS on the phone, or select with Volume and press Power.', flush=True)
            return
        fb(['oem', 'sm8750-touch-arm'], timeout=30)
        fb(['oem', 'run', '0'], timeout=30)
        result = dict(ram_boot_started=True, phone_partition_writes=False, automatic_return_seconds=0,
                      hardware_acceleration_verified=False, payload_sha256=meta['payload_sha256'],
                      loader=str(loader.relative_to(ROOT)), loader_sha256=digest(loader),
                      system=str(system), system_sha256=digest(system))
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
