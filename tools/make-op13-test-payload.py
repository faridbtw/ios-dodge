#!/usr/bin/env python3
"""Make a separate, RAM-only test payload; leave release images untouched.

Requires output from build-mesa-macos.py. gl=1 uses Mesa softpipe, not Adreno.
The original SM8750 GPU guard and all storage-write prohibitions are retained.
"""
import argparse
import ast
import hashlib
import json
import os
import plistlib
from pathlib import Path, PurePosixPath
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
APFS = ROOT / 'source/hackios-xnu-userland/tools/apfs'
sys.path.insert(0, str(APFS))
import apfs


def unpack(blob):
    magic, size = struct.unpack_from('<II', blob)
    if magic != 0x50554e58 or size != len(blob) - 8:
        raise ValueError('Invalid XNUP header')
    entries, offset = {}, 8
    while offset + 24 <= len(blob):
        name, size, reserved = struct.unpack_from('<16sII', blob, offset)
        offset += 24
        if not name.strip(b'\0'):
            if offset != len(blob): raise ValueError('Trailing XNUP data')
            return entries
        name = name.rstrip(b'\0').decode('ascii')
        if reserved or name in entries or offset + size > len(blob):
            raise ValueError('Invalid XNUP entry')
        entries[name] = blob[offset:offset + size]
        offset += (size + 15) & ~15
    raise ValueError('Missing XNUP terminator')


def pack(entries):
    body = b''
    for name, data in entries.items():
        body += struct.pack('<16sII', name.encode('ascii'), len(data), 0)
        body += data + b'\0' * (-len(data) % 16)
    body += b'\0' * 24
    result = struct.pack('<II', 0x50554e58, len(body)) + body
    if len(result) > 0x3f00000: raise ValueError('Payload exceeds loader 63 MiB limit')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--libraries', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cpus', type=int, choices=[1, 8], default=1)
    parser.add_argument('--gl', type=int, choices=[0, 1], default=0)
    parser.add_argument('--seconds', type=int, default=180)
    parser.add_argument('--kernel', type=Path, help='Explicit experimental kernel override')
    parser.add_argument('--wallpaper-jobs', type=Path,
        help='Stock job directory from fetch-ios-launch-jobs.py')
    parser.add_argument('--trace-gl', action='store_true', help='Log compositor shader sources')
    parser.add_argument('--no-benchmarks', action='store_true',
        help='Omit CPU and GL benchmark workloads from an interactive boot')
    parser.add_argument('--reduce-effects', type=Path,
        help='SpringBoard CPU rendering profile dylib: remove blur, reduce transparency/motion')
    parser.add_argument('--battery-adsp-probe', type=int, choices=[1, 2, 3],
        help='OnePlus13 battery diagnostic: shared-memory survey (1), firmware validation (2), retained live startup (3)')
    parser.add_argument('--battery-firmware', type=Path, help='Captured phone ADSP firmware directory for phase2')
    parser.add_argument('--battery-live', action='store_true',
        help='Enable the verified live ADSP battery transport in an eight-core gl=0 interactive RAM boot')
    parser.add_argument('--rest-jobs', action='store_true',
        help='Restore available on-demand jobs using the bundled mkjobs.py deny list')
    parser.add_argument('--fb-hz', type=int, choices=[60, 120],
        help='Advertised framebuffer/timer cadence only; does not program the physical panel')
    parser.add_argument('--display-probe', type=Path,
        help='Compiled fb_rate_test executable to include and run after startup')
    parser.add_argument('--ca-vbl', action='store_true', help='Use the existing CoreAnimation display-notification clock path')
    parser.add_argument('--ca-all-rates', action='store_true', help='Enable CoreAnimation frame-rate arbitration choices for an experimental test')
    parser.add_argument('--gen8-stage', type=int, choices=range(1, 18), help='Explicit staged Adreno 830 initialization probe (not acceleration)')
    parser.add_argument('--gen8-only', type=int, choices=[2], help='Run only the read-only GPUCC stage 2')
    parser.add_argument('--gen8-firmware', type=Path, help='Captured phone directory containing the four gen80000 firmware files')
    parser.add_argument('--bootcpu', type=int, choices=[6], help='Keep xnuloader on CPU0 and run a one-core probe on CPU6')
    parser.add_argument('--gen8-pause-ms', type=int, default=0)
    parser.add_argument('--probe-reset-seconds', type=int, help='Explicit cold-reset deadline for a one-core CPU6 RAM diagnostic')
    parser.add_argument('--gen8-retain', action='store_true', help='Retain HWSCHED resources until the explicit cold reset')
    parser.add_argument('--gen8-early-clocks', action='store_true', help='Use vendor operating clocks before loading GMU firmware, in a retained probe')
    parser.add_argument('--gen8-cp-aperture', action='store_true', help='Configure the vendor CP SMMU aperture for user context 0 in retained stage17')
    parser.add_argument('--gen8-direct-power', action='store_true', help='Probe core-start/DCVS/OOB without the firmware scheduler; retained stage14 only')
    parser.add_argument('--gen8-direct-ring', action='store_true', help='Probe direct CP initialization without firmware-scheduled submission; retained stage17 only')
    parser.add_argument('--gen8-operating-clocks', action='store_true', help='Opt-in vendor 500 MHz GMU/200 MHz hub clock experiment in stage17')
    parser.add_argument('--gen8-raw-nop-test', action='store_true', help='Diagnostic NOP only, before CP_ME_INIT; stage17 remains incomplete')
    parser.add_argument('--gen8-protected', action='store_true', help='Enable real protected context-record allocation in retained stage14')
    parser.add_argument('--gen8-secure-probe', action='store_true', help='Probe QTEE bridge ABI instead of HWSCHED START; no GPU memory ownership changes')
    parser.add_argument('--gen8-secure-assign-test', action='store_true', help='Test ownership roundtrip of one disposable page; no GPU buffers assigned')
    args = parser.parse_args()
    if args.battery_live and (args.battery_adsp_probe or args.gen8_stage or args.cpus != 8 or args.gl != 0 or
            args.seconds != 0 or args.bootcpu is not None or args.probe_reset_seconds or not args.kernel or not args.battery_firmware):
        parser.error('--battery-live needs eight cores, gl=0, no return timer, an explicit kernel and phone firmware')
    if args.battery_adsp_probe and (args.gen8_stage or args.cpus != 1 or args.gl != 0 or args.bootcpu != 6 or
            not args.kernel or not args.probe_reset_seconds or not 30 <= args.probe_reset_seconds <= 120 or args.seconds <= args.probe_reset_seconds):
        parser.error('--battery-adsp-probe needs CPU6/one core, gl=0, explicit kernel and a 30..120s reset before loader return')
    if args.battery_adsp_probe and args.battery_adsp_probe >= 2 and not args.battery_firmware:
        parser.error('Battery phases2/3 need --battery-firmware captured from this phone')
    if args.battery_adsp_probe == 3 and args.probe_reset_seconds < 90:
        parser.error('Live battery startup needs a 90..120s cold-reset deadline')
    if args.wallpaper_jobs and not (args.wallpaper_jobs / 'System/Library').is_dir():
        parser.error('--wallpaper-jobs must contain System/Library (use apple-stock-jobs)')
    if args.rest_jobs and not args.wallpaper_jobs:
        parser.error('--rest-jobs requires --wallpaper-jobs')
    if args.seconds != 0 and not 30 <= args.seconds <= 600:
        parser.error('--seconds must be 0 (no automatic return) or 30..600')
    if args.gen8_cp_aperture and (args.gen8_stage != 17 or not args.gen8_retain or not (args.gen8_protected or args.gen8_direct_ring)):
        parser.error('--gen8-cp-aperture requires retained protected stage17')
    if args.gen8_direct_power and (args.gen8_stage != 14 or not args.gen8_retain or args.gen8_protected or args.gen8_secure_probe or args.gen8_secure_assign_test):
        parser.error('--gen8-direct-power requires retained stage14 without protected-memory diagnostics')
    if args.gen8_direct_ring and (args.gen8_stage != 17 or not args.gen8_retain or args.gen8_protected or args.gen8_secure_probe or args.gen8_secure_assign_test or args.gen8_direct_power or args.gen8_raw_nop_test):
        parser.error('--gen8-direct-ring requires retained stage17 without protected-memory/RAW diagnostics')
    if args.gen8_early_clocks and (not args.gen8_stage or args.gen8_stage < 14 or not args.gen8_retain or not (args.gen8_protected or args.gen8_direct_power or args.gen8_direct_ring) or args.gen8_operating_clocks):
        parser.error('--gen8-early-clocks requires retained protected stage14+ and excludes late clocks')
    if args.gen8_operating_clocks and (args.gen8_stage != 17 or not args.gen8_retain or not args.gen8_protected):
        parser.error('--gen8-operating-clocks requires retained protected stage17')
    if args.gen8_raw_nop_test and (args.gen8_stage != 17 or not args.gen8_retain or not args.gen8_protected):
        parser.error('--gen8-raw-nop-test requires retained protected stage17')
    if sum([args.gen8_protected, args.gen8_secure_probe, args.gen8_secure_assign_test]) > 1:
        parser.error('Select one protected-memory diagnostic mode')
    if (args.gen8_secure_probe or args.gen8_secure_assign_test) and (not args.gen8_retain or args.gen8_stage != 14):
        parser.error('--gen8-secure-probe requires retained stage14 and cold-reset recovery')
    if args.gen8_protected and (not args.gen8_retain or not args.gen8_stage or args.gen8_stage < 14):
        parser.error('--gen8-protected requires a retained stage14+ probe')
    if args.gen8_retain:
        if (args.gen8_stage is None or args.gen8_stage < 14) or args.bootcpu != 6 or args.cpus != 1 or not args.probe_reset_seconds or args.probe_reset_seconds < 20 or args.seconds <= args.probe_reset_seconds:
            parser.error('--gen8-retain requires stage14+, CPU6/one core and a 20..120s cold-reset deadline before loader return')
    if args.gen8_stage and args.gen8_stage >= 14 and not args.gen8_retain:
        parser.error('stage14 requires --gen8-retain with cold-reset recovery')
    if args.probe_reset_seconds is not None:
        if args.bootcpu != 6 or args.cpus != 1 or not 5 <= args.probe_reset_seconds <= 120:
            parser.error('--probe-reset-seconds requires --bootcpu 6 --cpus 1 and a 5..120 second deadline')
    if args.bootcpu is not None and args.cpus != 1:
        parser.error('--bootcpu requires --cpus 1 for diagnostics')
    if not 0 <= args.gen8_pause_ms <= 5000:
        parser.error('--gen8-pause-ms must be 0..5000')
    if args.gen8_stage and (not args.kernel or b'adreno8: Dodge currently' not in args.kernel.read_bytes()):
        parser.error('--gen8-stage needs the separately built Dodge Gen8 kernel')
    if args.gen8_only and args.gen8_stage != 2:
        parser.error('--gen8-only 2 requires --gen8-stage 2')
    if args.gen8_stage and args.gen8_stage >= 9 and not args.gen8_firmware:
        parser.error('stages 9+ require --gen8-firmware from this phone')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    root = out / 'root'
    if root.exists(): raise SystemExit('Output already has a root directory; choose a new output')
    root.mkdir()
    source = ROOT / 'touch-v15-payload.bin'
    entries = unpack(source.read_bytes())
    if entries['kernel'] != (ROOT / 'build-kernel-match.bin').read_bytes():
        raise SystemExit('Release kernel and payload kernel differ')
    if args.kernel:
        entries['kernel'] = args.kernel.read_bytes()
    original = out / 'original-root.apfs'
    original.write_bytes(entries['ramdisk'])
    container = apfs.Container(str(original))
    volume = apfs.pick_volume(container, None)
    manifest, unchanged = [], {}
    for path, _, oid, _ in apfs.walk(volume, 2):
        parts = PurePosixPath(path).parts[1:]
        if '..' in parts: raise ValueError('Unsafe APFS path')
        inode = volume.inode(oid)
        destination = root.joinpath(*parts)
        if inode.is_dir: destination.mkdir()
        elif inode.is_link: destination.symlink_to(os.fsdecode(inode.contents()))
        elif inode.is_reg:
            destination.write_bytes(inode.contents())
            unchanged[path] = hashlib.sha256(inode.contents()).hexdigest()
        else: raise ValueError('Unsupported root inode')
        if not inode.is_link: destination.chmod(inode.mode & 0o7777)
        manifest.append(f'{inode.mode:o}\t{inode.owner}\t{inode.group}\t{path.lstrip("/")}')
    libraries = args.libraries.resolve()
    replacements = {'bin/op13_cpu_check': 'op13_cpu_check'}
    if args.gl:
        replacements.update({'lib/libOSMesa.8.dylib': 'libOSMesa.8.dylib',
            'lib/libredwood-gl.dylib': 'libredwood-gl.dylib',
            'lib/libredwood.dylib': 'libredwood.dylib', 'bin/gl_test': 'gl_test'})
    for destination, name in replacements.items():
        shutil.copy2(libraries / name, root / destination)
    if args.display_probe:
        shutil.copy2(args.display_probe, root / 'bin/fb_rate_test')
    jobs = root / 'etc/rc.jobs'
    text = jobs.read_text().replace('/redwood/bin/cpu_test 240 &',
        '' if args.no_benchmarks else f'/redwood/bin/op13_cpu_check {args.cpus} &')
    if args.gl and not args.no_benchmarks: text += '\n/redwood/bin/gl_test 0 320 240\n'
    if args.display_probe: text += '\n/redwood/bin/fb_rate_test 5 40 &\n'
    jobs.write_text(text)
    changed = set('/' + name for name in replacements) | {'/etc/rc.jobs'}
    if args.reduce_effects:
        destination = 'lib/libop13-ui-performance.dylib'
        shutil.copy2(args.reduce_effects, root / destination)
        manifest.append(f'100755\t0\t0\t{destination}')
        changed.add('/' + destination)
        injection = root / 'etc/inject.conf'
        config = injection.read_text().rstrip() + '\n/redwood/' + destination + ' SpringBoard\n'
        if len(config.encode()) >= 512: raise ValueError('Kernel injection configuration exceeds 512 bytes')
        injection.write_text(config)
        changed.add('/etc/inject.conf')
        prefs = 'var/mobile/Library/Preferences/com.apple.Accessibility.plist'
        pref_path = root / prefs
        values = plistlib.loads(pref_path.read_bytes()) if pref_path.exists() else {}
        values.update(ReduceTransparencyEnabled=True, ReduceMotionEnabled=True)
        if not pref_path.exists(): manifest.append(f'100644\t501\t501\t{prefs}')
        pref_path.write_bytes(plistlib.dumps(values, fmt=plistlib.FMT_BINARY))
        changed.add('/' + prefs)
    if args.gen8_firmware:
        directory = root / 'fw'
        if not directory.exists():
            directory.mkdir(mode=0o755)
            manifest.append('40755\t0\t0\tfw')
        for name in ['gen80000_gmu.bin', 'gen80000_sqe.fw', 'gen80000_aqe.fw', 'gen80000_zap.mbn']:
            source_fw = args.gen8_firmware / name
            if not source_fw.is_file(): raise ValueError(f'Missing firmware {name}')
            dest = directory / name
            dest.write_bytes(source_fw.read_bytes()); dest.chmod(0o644)
            manifest.append(f'100644\t0\t0\tfw/{name}')
            changed.add('/fw/' + name)
    if args.display_probe: changed.add('/bin/fb_rate_test')
    if args.battery_firmware:
        directory = root / 'fw'
        if not directory.exists():
            directory.mkdir(mode=0o755)
            manifest.append('40755\t0\t0\tfw')
        import re
        for source_fw in sorted(args.battery_firmware.iterdir()):
            if not source_fw.is_file() or not re.fullmatch(r'adsp(?:_dtb)?\.(?:mdt|b[0-9]{2})', source_fw.name): continue
            dest = directory / source_fw.name
            dest.write_bytes(source_fw.read_bytes()); dest.chmod(0o644)
            manifest.append(f'100644\t0\t0\tfw/{source_fw.name}')
            changed.add('/fw/' + source_fw.name)
    if args.wallpaper_jobs:
        system = apfs.Container(str(ROOT / 'system.apfs'))
        system_volume = apfs.pick_volume(system, None)
        labels = ['com.apple.PosterBoard', 'com.apple.PaperBoard', 'com.apple.mediaserverd']
        if args.rest_jobs:
            tree = ast.parse((ROOT / 'source/hackios-xnu-userland/userland/mkjobs.py').read_text())
            deny = next(ast.literal_eval(node.value) for node in tree.body
                if isinstance(node, ast.Assign) and isinstance(node.value, ast.Tuple)
                and any(isinstance(target, ast.Name) and target.id == 'DENY' for target in node.targets))
            labels = []
            for path in sorted((args.wallpaper_jobs / 'System/Library').glob('*/*.plist')):
                label = path.stem
                if (root / 'LaunchDaemons' / path.name).exists() or any(word in label for word in deny):
                    continue
                job = plistlib.loads(path.read_bytes())
                program = job.get('Program') or (job.get('ProgramArguments') or [''])[0]
                if not program or not job.get('MachServices'): continue
                try:
                    if system_volume.lookup(program) is None: continue
                except apfs.ApfsError: continue
                labels.append(label)
        for label in labels:
            matches = list((args.wallpaper_jobs / 'System/Library').glob('*/' + label + '.plist'))
            if len(matches) != 1: raise ValueError(f'Missing/ambiguous stock job: {label}')
            job = plistlib.loads(matches[0].read_bytes())
            if not isinstance(job.get('Label'), str) or not job.get('MachServices') or (
                    not args.rest_jobs and job['Label'] != label):
                raise ValueError(f'Invalid stock job: {label}')
            program = job.get('Program') or job['ProgramArguments'][0]
            if system_volume.lookup(program) is None:
                raise ValueError(f'Stock job executable is absent: {program}')
            for key in ['RunAtLoad', 'KeepAlive', 'StartInterval', 'StartCalendarInterval',
                        'WatchPaths', 'QueueDirectories', 'StartOnMount', 'LaunchEvents']:
                job.pop(key, None)
            job['StandardErrorPath'] = job['StandardOutPath'] = '/dev/console'
            if label in ['com.apple.PosterBoard', 'com.apple.PaperBoard']:
                job.setdefault('EnvironmentVariables', {})['OS_ACTIVITY_DT_MODE'] = 'YES'
            destination = f'LaunchDaemons/{label}.plist'
            (root / destination).write_bytes(plistlib.dumps(job, fmt=plistlib.FMT_BINARY))
            changed.add('/' + destination)
    for path, digest in unchanged.items():
        if path not in changed and hashlib.sha256((root / path.lstrip('/')).read_bytes()).hexdigest() != digest:
            raise ValueError(f'Unexpected root file change: {path}')
    metadata = out / 'root.manifest'
    metadata.write_text('\n'.join(manifest) + '\n')
    image = out / 'root.apfs'
    subprocess.run([sys.executable, str(APFS / 'mkapfs.py'), '-z', '-o', str(image),
        f'redwood={root},manifest={metadata}'], check=True)
    # Read every file back through the same APFS implementation the bundle uses.
    new_container = apfs.Container(str(image))
    new_volume = apfs.pick_volume(new_container, None)
    for path, _, oid, _ in apfs.walk(new_volume, 2):
        inode = new_volume.inode(oid)
        if inode.is_reg and inode.contents() != (root / path.lstrip('/')).read_bytes():
            raise ValueError(f'APFS round-trip mismatch: {path}')
    arguments = dict(token.split('=', 1) for token in entries['cmdline'].rstrip(b'\0').decode().split())
    arguments.update(cpus=str(args.cpus), gl=str(args.gl), loader_return_after=str(args.seconds), gicr_stride='0x40000')
    if args.battery_adsp_probe:
        arguments['op13_batt'] = str(args.battery_adsp_probe)
    if args.battery_live:
        arguments['op13_batt'] = '4'
    if args.gen8_stage:
        arguments.update(adreno8=str(args.gen8_stage), adreno8_pause=str(args.gen8_pause_ms), adreno8_watch='0', adreno8_return='1', adreno8_keep='0')
        if args.gen8_retain: arguments.update(adreno8_keep='1', adreno8_return='0')
        if args.gen8_early_clocks: arguments['a8_clocks'] = '2'
        if args.gen8_cp_aperture: arguments['a8_ap'] = '1'
        if args.gen8_direct_power: arguments['a8_direct'] = '1'
        if args.gen8_direct_ring: arguments['a8_direct'] = '2'
        if args.gen8_operating_clocks: arguments['a8_clocks'] = '1'
        if args.gen8_raw_nop_test: arguments['a8_nop'] = '1'
        if args.gen8_protected: arguments['adreno8_protected'] = '1'
        if args.gen8_secure_probe: arguments['adreno8_secure_probe'] = '1'
        if args.gen8_secure_assign_test: arguments['adreno8_secure_probe'] = '2'
        if args.gen8_only: arguments['adreno8_only'] = str(args.gen8_only)
    if args.bootcpu is not None: arguments['bootcpu'] = str(args.bootcpu)
    if args.gen8_retain:
        if (args.gen8_stage is None or args.gen8_stage < 14) or args.bootcpu != 6 or args.cpus != 1 or not args.probe_reset_seconds or args.probe_reset_seconds < 20 or args.seconds <= args.probe_reset_seconds:
            parser.error('--gen8-retain requires stage14+, CPU6/one core and a 20..120s cold-reset deadline before loader return')
    if args.gen8_stage and args.gen8_stage >= 14 and not args.gen8_retain:
        parser.error('stage14 requires --gen8-retain with cold-reset recovery')
    if args.probe_reset_seconds is not None: arguments['sm8750_test_reset_seconds'] = str(args.probe_reset_seconds)
    if args.fb_hz is not None: arguments['fb_hz'] = str(args.fb_hz)
    if args.ca_vbl: arguments['ca_vbl'] = '1'
    if args.ca_all_rates: arguments['caenv'] = 'CA_ENABLE_ALL_FRAME_RATES=1'
    if args.trace_gl: arguments['glshaders'] = '1'
    for key in ['ufs', 'ufs_write', 'var_store', 'gpu', 'adreno', 'metal', 'cpufreq']:
        if arguments.get(key) != '0': raise ValueError(f'Unsafe or unsupported boot argument {key}')
    if args.gen8_stage:
        arguments.pop('epoch', None)
        arguments.pop('display_probe', None)
    command_line = ' '.join(f'{key}={value}' for key, value in arguments.items())
    if len(command_line.encode()) > 608 - 192:
        parser.error('Command line exceeds the touch-runtime 416-byte lease budget')
    entries['ramdisk'] = image.read_bytes()
    entries['cmdline'] = (command_line + '\0').encode()
    payload = pack(entries)
    assert unpack(payload) == entries
    (out / 'payload.bin').write_bytes(payload)
    (out / 'BUILD.json').write_text(json.dumps({'experimental': True, 'device_validated': False,
        'reported_refresh_hz': args.fb_hz or 60, 'physical_refresh_hz_validated': False,
        'cpu_count_requested': args.cpus, 'renderer': 'Mesa softpipe' if args.gl else 'CoreAnimation software',
        'hardware_gpu_acceleration': False, 'cmdline': entries['cmdline'].rstrip(b'\0').decode(),
        'live_battery_enabled': args.battery_live,
        'source_payload_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
        'kernel_sha256': hashlib.sha256(entries['kernel']).hexdigest(),
        'kernel_override': str(args.kernel) if args.kernel else None,
        'payload_sha256': hashlib.sha256(payload).hexdigest(), 'size': len(payload),
        'modified_or_added_root_files': sorted(changed)}, indent=2) + '\n')
    print(f'Built and read back: {out / "payload.bin"} ({len(payload)} bytes)')


if __name__ == '__main__':
    main()
