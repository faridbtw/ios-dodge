#!/usr/bin/env python3
"""Run a retained Gen8 RAM diagnostic with live USB capture and verified cold-reset recovery."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import runpy
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
LOADER = ROOT / 'experimental/adreno120/dodge-display-query.img'
LOADER_HASH = '29f2d04a2db4a684d6436564e3f3a916fbde01bb7081a9604882665b4052a86e'


def main():
    global LOADER, LOADER_HASH
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--audio-probe', action='store_true', help='Use the bounded amplifier query loader and AudioReach graph probe')
    parser.add_argument('--battery-probe', action='store_true', help='Run a OnePlus13 ADSP battery diagnostic instead of a GPU probe')
    parser.add_argument('--reuse-armed-loader', action='store_true', help='Recover only a documented touch-runtime command-line refusal with its existing lease')
    parser.add_argument('--payload', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    payload = args.payload.resolve()
    entries = runpy.run_path(str(ROOT / 'tools/make-op13-test-payload.py'))['unpack'](payload.read_bytes())
    boot = dict(t.split('=', 1) for t in entries['cmdline'].rstrip(b'\0').decode().split())
    if args.audio_probe:
        if not args.battery_probe or boot.get('op13_audio') not in ['2','3','4','5','6','7','8','9'] or args.reuse_armed_loader:
            raise SystemExit('Audio probe requires a fresh timed battery phase3/graph test')
        LOADER = ROOT / 'experimental/audio/turtle-diagnostic-v1/dodge-audio-uefi.img'
        manifest = json.loads(LOADER.with_suffix('.json').read_text())
        assert manifest['image_sha256'] == hashlib.sha256(LOADER.read_bytes()).hexdigest()
        assert manifest['loader_executables_unchanged'] and manifest['other_inner_ffs_unchanged']
        assert manifest['changed_inner_ffs'] == ['7bb28b99-61bb-11d5-9a5d-0090273fc14d']
        base = (ROOT / manifest['source']).resolve()
        assert base.is_relative_to(ROOT.resolve())
        assert hashlib.sha256(base.read_bytes()).hexdigest() == manifest['source_sha256']
        packaging = json.loads(base.with_suffix('.json').read_text())
        assert packaging['image_sha256'] == manifest['source_sha256']
        assert packaging['source_sha256'] == 'f26410ca510dcb7352f5c0b814ca435aaefe05bdc3ed1697ef5091b79d588cb8'
        assert all(packaging[k] for k in ['other_ffs_unchanged','bootshim_unchanged','dtb_unchanged','ramdisk_and_signature_bytes_unchanged'])
        LOADER_HASH = manifest['image_sha256']
    if len(entries['cmdline'].rstrip(b'\0')) > 416:
        raise SystemExit('Command line exceeds the loader touch-lease budget')
    for key in ['gpu', 'adreno', 'metal', 'gl', 'ufs', 'ufs_write', 'var_store', 'cpufreq', 'wifi', 'adsp']:
        if boot.get(key) != '0': raise SystemExit(f'Unexpected {key}; refusing diagnostic')
    expected = {'cpus': '1', 'bootcpu': '6', 'adreno8_keep': '1',
        'adreno8_return': '0', 'sm8750_gpu': '1', 'gicr_stride': '0x40000'}
    if args.battery_probe:
        expected.pop('adreno8_keep'); expected.pop('adreno8_return')
    for key, value in expected.items():
        if boot.get(key) != value: raise SystemExit(f'Expected {key}={value}')
    if args.battery_probe and (boot.get('op13_batt') not in ['1', '2', '3'] or 'adreno8' in boot):
        raise SystemExit('Expected battery phase1/2/3 without GPU probes')
    if not args.battery_probe and not 14 <= int(boot.get('adreno8','0')) <= 17:
        raise SystemExit('Expected a gated stage14..17 diagnostic')
    if not args.battery_probe and int(boot['adreno8']) > 14 and boot.get('adreno8_protected') != '1' and not (boot.get('a8_direct') == '2' and boot['adreno8'] == '17'):
        raise SystemExit('Later stages require the protected allocator')
    reset = int(boot.get('sm8750_test_reset_seconds', '0'))
    if args.battery_probe and boot.get('op13_batt') == '3' and reset < 90:
        raise SystemExit('Live battery startup needs 90..120s reset recovery')
    if not 20 <= reset <= 120 or int(boot.get('loader_return_after', '0')) <= reset:
        raise SystemExit('Expected cold reset before the ordinary return deadline')
    if b'adreno8: Dodge currently' not in entries['kernel'] or b'PSCI cold reset scheduled' not in entries['kernel']:
        raise SystemExit('Expected the gated Dodge kernel and cold-reset implementation')
    if hashlib.sha256(LOADER.read_bytes()).hexdigest() != LOADER_HASH:
        raise SystemExit('Unexpected diagnostic loader image')
    build = json.loads((payload.parent / 'BUILD.json').read_text())
    if build.get('invalid'):raise SystemExit('This diagnostic artifact is marked invalid')
    if boot.get('op13_audio_sd')=='2' and b'secondary MI2S data line=' not in entries['kernel']:
        raise SystemExit('Kernel lacks selectable MI2S data line support')
    if build['payload_sha256'] != hashlib.sha256(payload.read_bytes()).hexdigest():
        raise SystemExit('Payload differs from its build manifest')
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    receipts = []

    def command(argv, timeout=8, check=True):
        started = time.monotonic()
        try:
            r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
            receipts.append({'command': argv, 'exit': r.returncode,
                'seconds': time.monotonic() - started, 'output': r.stdout + r.stderr})
            if check: r.check_returncode()
            return r.stdout + r.stderr, r.returncode
        except subprocess.TimeoutExpired:
            receipts.append({'command': argv, 'timeout': timeout})
            raise
        finally:
            (out / 'receipts.json').write_text(json.dumps(receipts, indent=2) + '\n')

    def fb(argv, timeout=8, serial='xnuloader', check=True):
        # Fixed operations: RAM boot, RAM staging, read diagnostics, and start the probe.
        allowed = (argv[:1] in [['getvar'], ['get_staged']] or
            argv in [['stage', str(payload)], ['stage', str(ROOT / 'system.apfs')],
                ['boot', str(LOADER)], ['oem', 'ramdisk'], ['oem', 'sm8750-touch-arm'],
                ['oem', 'run', '0'], ['oem', 'log'], ['oem','sm8750-touch-cleanup'], ['oem','sm8750-touch-status']])
        if args.audio_probe and argv == ['oem','op13-audio-amp-query']: allowed = True
        if args.audio_probe and boot.get('op13_audio') in ['4','5','6','7','8','9'] and argv == ['oem','op13-audio-cold-reset']: allowed = True
        if not allowed: raise ValueError('Unexpected fastboot operation')
        return command(['fastboot', '-s', serial, *argv], timeout, check)

    state, code = command(['adb', '-s', args.serial, 'get-state'], check=False)
    if args.reuse_armed_loader and code == 0:
        raise SystemExit('Armed-loader recovery requires the resident loader, not Android')
    if code == 0 and state.strip() == 'device':
        identity, _ = command(['adb', '-s', args.serial, 'shell', 'getprop ro.board.platform'])
        if identity.strip() != 'sun': raise SystemExit('Connected Android is not the matched sun board')
        for _ in range(30):
            ready, _ = command(['adb', '-s', args.serial, 'shell', 'getprop sys.boot_completed'])
            if ready.strip() == '1': break
            time.sleep(1)
        else: raise SystemExit('Android startup has not completed; no reboot attempted')
        command(['adb', '-s', args.serial, 'reboot', 'bootloader'])
        for _ in range(40):
            time.sleep(0.5)
            try:
                text, code = fb(['getvar', 'product'], 2, args.serial, False)
                if code == 0 and 'sun' in text: break
            except subprocess.TimeoutExpired: pass
        else: raise SystemExit('Matched bootloader did not enumerate')
        fb(['boot', str(LOADER)], 20, args.serial)
    for _ in range(50):
        time.sleep(0.5)
        try:
            text, code = fb(['getvar', 'product'], 5, check=False)
            if code == 0 and 'xnuloader' in text: break
        except subprocess.TimeoutExpired: pass
    else: raise SystemExit('RAM loader did not enumerate')
    text, _ = fb(['getvar', 'max-download-size'])
    if '0x03f00000' not in text: raise SystemExit('Resident-root transfer already armed; refusing staging')
    if args.reuse_armed_loader:
        status, _ = fb(['getvar', 'result'])
        if 'not-started' not in status: raise SystemExit('Only a refused, unstarted kernel can reuse the touch lease')
        fb(['oem', 'log']); proof = out / 'prior-loader-refusal.log'
        fb(['get_staged', str(proof)])
        if 'touch-runtime refused incompatible payload arguments' not in proof.read_text(errors='replace'):
            raise SystemExit('Expected the documented command-line guard refusal')
        fb(['oem','sm8750-touch-status'])
        fb(['oem','sm8750-touch-cleanup'])
        fb(['stage', str(payload)], 15)
        fb(['oem','sm8750-touch-arm'],15)
        print('Shortened payload staged; resident root retained and touch lease rearmed after cleanup', flush=True)
    else:
        if args.audio_probe:
            try:
                fb(['oem','op13-audio-amp-query'])
            except subprocess.CalledProcessError:
                print('Amplifier identity incomplete; continuing only the silent graph open/close probe', flush=True)
            finally:
                fb(['oem','log']); fb(['get_staged',str(out/'amplifiers.log')])
        fb(['stage', str(payload)], 15)
        fb(['oem', 'ramdisk'])
        fb(['stage', str(ROOT / 'system.apfs')], 120)
        print('Payload and system image staged in RAM', flush=True)
        fb(['oem', 'sm8750-touch-arm'], 15)
    fb(['oem', 'run', '0'])
    probe_start = time.monotonic()
    deadline = time.monotonic() + reset + 10
    last = None
    index = 0
    smmu_wait_since = None
    smmu_forced_reset = False
    while time.monotonic() < deadline:
        time.sleep(0.7)
        try:
            text, code = fb(['getvar', 'result'], 2, check=False)
            if code: continue
            fb(['oem', 'log'])
            path = out / f'live-{index:03d}.log'; index += 1
            fb(['get_staged', str(path)])
            if path.exists() and path.stat().st_size:
                last = path
                capture = path.read_text(errors='replace')
                if (('native about to read IDR0' in capture and 'native IDR0=' not in capture) or
                    ('programming private context next' in capture and 'private mapping result=' not in capture)):
                    if smmu_wait_since is None: smmu_wait_since = time.monotonic()
                    if time.monotonic() - smmu_wait_since > 6:
                        print('Native SMMU read did not complete; issuing diagnostic cold reset', flush=True)
                        fb(['oem','op13-audio-cold-reset'])
                        smmu_forced_reset = True
                        break
        except (subprocess.TimeoutExpired, subprocess.CalledProcessError):
            # A live loader can temporarily be busy during power/MMIO operations.
            # Keep retrying within the explicit reset deadline to retain later logs.
            continue
    if last:
        log = last.read_text(errors='replace')
        (out / 'last-kernel.log').write_bytes(last.read_bytes())
        lines = [l for l in log.splitlines() if l.startswith(('adreno8:', 'op13-audio:'))]
        print('\n'.join(lines[-18:]), flush=True)
    else: log = ''
    recovered = False
    recovery_elapsed = android_uptime = None
    deadline = time.monotonic() + 65
    while time.monotonic() < deadline:
        text, code = command(['adb', '-s', args.serial, 'get-state'], 4, False)
        if code == 0 and text.strip() == 'device':
            identity, _ = command(['adb', '-s', args.serial, 'shell',
                'getprop ro.board.platform; cat /proc/uptime'])
            if identity.splitlines()[0] != 'sun': raise SystemExit('Unexpected Android after reset')
            (out / 'android-after-reset.txt').write_text(identity)
            recovery_elapsed = time.monotonic() - probe_start
            android_uptime = float(identity.splitlines()[1].split()[0])
            recovered = True; break
        time.sleep(1)
    live_samples = re.findall(r'op13-battery: live sample ([^\n]+)', log)
    battery_ok = (len(live_samples) >= 2 if boot.get('op13_batt') == '3' else f"op13-battery: phase {boot.get('op13_batt', '0')} passed" in log)
    result = {'requested_stage': int(boot.get('adreno8', '0')),
        'battery_probe_phase': int(boot.get('op13_batt', '0')),
        'audio_spf_ready': 'AudioReach SPF ready=1' in log,
        'audio_graph_roundtrip_passed': 'graph probe open=1 close=1' in log,
        'audio_clock_probe_started': 'media=1 core=1 clock=1 started=1' in log,
        'native_smmu_read_forced_reset': smmu_forced_reset,
        'native_smmu_read_completed': 'native IDR0=' in log,
        'audio_phase': int(boot.get('op13_audio','0')),
        'audio_private_mapping_ready': 'private mapping result=0' in log,
        'audio_dsp_mapping_ready': 'DSP map=1' in log,
        'audio_muted_amp_profile_ready': 'native muted profile result=0' in log,
        'audio_quiet_amp_started': 'quiet start result=0' in log,
        'audio_tone_sample_consumed': 'tone sample consumed=1' in log,
        'audio_silent_sample_consumed': 'silent sample consumed=1' in log,
        'battery_probe_passed': battery_ok,
        'battery_live_samples': live_samples,
        'direct_power_probe_only': boot.get('a8_direct') == '1',
        'direct_power_probe_passed': 'direct-power core firmware, DCVS and OOB passed' in log,
        'direct_ring_probe_only': boot.get('a8_direct') == '2',
        'direct_ring_cp_init_completed': 'direct-ring CP_ME_INIT consumed 10/10 and idle' in log,
        'raw_nop_probe_only': boot.get('a8_nop', boot.get('adreno8_raw_nop_probe')) == '1',
        'raw_nop_acknowledged_and_idle': 'RAW NOP acknowledged and idle' in log,
        'requested_stage_completed': (('finite playback complete=1' in log if boot.get('op13_audio')=='9' else ('quiet start result=0' in log and 'tone sample consumed=1' in log) if boot.get('op13_audio')=='8' else ('native muted profile result=0' in log and 'silent sample consumed=1' in log) if boot.get('op13_audio')=='7' else 'silent sample consumed=1' in log if boot.get('op13_audio')=='6' else 'private mapping result=0' in log if boot.get('op13_audio')=='5' else 'native plan=0' in log if boot.get('op13_audio')=='4' else 'media=1 core=1 clock=1 started=1' in log if boot.get('op13_audio')=='3' else 'graph probe open=1 close=1' in log) if args.audio_probe else battery_ok if args.battery_probe else f"adreno8: {boot['adreno8']}: completed" in log),
        'estimated_reset_after_seconds': round(recovery_elapsed - android_uptime, 2) if recovered else None,
        'completed_stages': sorted(set(map(int, re.findall(r'adreno8: (\d+): completed', log)))),
        'failed_stages': sorted(set(map(int, re.findall(r'adreno8: (\d+): FAILED', log)))),
        'secure_bridge_probe_only': boot.get('adreno8_secure_probe') in ['1', '2'],
        'hwsched_start_acknowledged': 'HWSCHED START acknowledged' in log,
        'cold_reset_recovered_android': recovered, 'hardware_rendering_proven': False,
        'phone_partition_writes': False, 'payload_sha256': build['payload_sha256']}
    (out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2), flush=True)
    return 0 if recovered else 1


if __name__ == '__main__': raise SystemExit(main())
