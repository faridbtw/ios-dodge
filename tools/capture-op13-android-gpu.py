#!/usr/bin/env python3
"""Capture the stock KGSL cold-start trace with temporary, restored debug settings."""
import argparse
from pathlib import Path
import shlex
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--serial', required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
if a.output.exists():
    raise SystemExit('Output exists; refusing to replace evidence')

# Vendor adreno_debugfs.c documents warmboot=0 as the supported cold-start
# switch. kgsl_pwrctrl.c defines idle_timer in microseconds. Both are volatile.
query = r'''set -eu
[ "$(getprop ro.board.platform)" = sun ]
[ "$(id -u)" = 0 ]
MOUNTED=0
if ! grep -q ' /sys/kernel/debug debugfs ' /proc/mounts; then
    mount -t debugfs debugfs /sys/kernel/debug
    MOUNTED=1
fi
T=/sys/kernel/tracing/instances/op13_gpu_cold_capture
W=/sys/kernel/debug/kgsl/kgsl-3d0/warmboot
I=/sys/class/kgsl/kgsl-3d0/idle_timer
OLD_W=$(cat "$W")
OLD_I=$(cat "$I")
CREATED=0
cleanup() {
    if [ "$CREATED" = 1 ]; then
        echo 0 > "$T/tracing_on"
        echo 0 > "$T/events/enable"
    fi
    echo "$OLD_I" > "$I"
    echo "$OLD_W" > "$W"
    echo "RESTORED warmboot=$(cat "$W") idle_timer=$(cat "$I")"
    if [ "$CREATED" = 1 ]; then rmdir "$T"; fi
    if [ "$MOUNTED" = 1 ]; then umount /sys/kernel/debug; fi
}
trap cleanup EXIT
mkdir "$T"
CREATED=1
echo 4096 > "$T/buffer_size_kb"
for E in kgsl_regwrite kgsl_hfi_send kgsl_hfi_receive kgsl_gmu_oob_set kgsl_gmu_oob_clear kgsl_pwr_set_state kgsl_gen8_irq_status; do
    echo 1 > "$T/events/kgsl/$E/enable"
done
echo "ORIGINAL warmboot=$OLD_W idle_timer=$OLD_I"
echo 1 > "$T/tracing_on"
echo 1000000 > "$I"
echo 0 > "$W"
input keyevent KEYCODE_WAKEUP
for C in gpu_cc_pll0 gpu_cc_gmu_clk_src gpu_cc_hub_clk_src gpu_cc_cx_gmu_clk gpu_cc_gx_gmu_clk gpu_cc_hlos1_vote_gpu_smmu_clk; do
    echo "CLOCK $C"
    cat /sys/kernel/debug/clk/$C/clk_print_regs
done
sleep 1
echo 0 > "$T/tracing_on"
echo TRACE_START
cat "$T/trace"
'''
r = subprocess.run(['adb', '-s', a.serial, 'shell', 'su -c ' + shlex.quote(query)],
                   capture_output=True, timeout=45)
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_bytes(r.stdout + b'\nSTDERR\n' + r.stderr)
print(f'Captured {len(r.stdout)} bytes; remote exit {r.returncode}; {a.output}')
if r.returncode:
    print(r.stderr.decode(errors='replace'))
raise SystemExit(r.returncode)
