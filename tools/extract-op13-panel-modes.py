#!/usr/bin/env python3
"""Extract Dodge panel timing and DSI mode-switch data without hardware access.

A mode in the vendor source proves support, not that the connected panel is
using it. Never select a mode until the physical panel variant is identified.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT / 'source/vendor-oneplus-sm8750-modules'
BASE = REPO / 'vendor/qcom/opensource/display-devicetree/oplus'
OVERLAY = BASE / 'dodge-23821-display-sun-overlay.dtsi'


def clean(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def block(text, start):
    depth = 1
    offset = start
    while offset < len(text) and depth:
        depth += (text[offset] == '{') - (text[offset] == '}')
        offset += 1
    if depth: raise ValueError('Unclosed timing node')
    return text[start:offset - 1]


def number(text, property_):
    match = re.search(re.escape(property_) + r'\s*=\s*<\s*(0x[\da-fA-F]+|\d+)\s*>', text)
    return int(match[1], 0) if match else None


def commands(text):
    match = re.search(r'qcom,mdss-dsi-timing-switch-command\s*=\s*\[([^]]*)\]', text)
    if not match: return []
    raw = bytes(int(token, 16) for token in match[1].split())
    offset, packets = 0, []
    while offset < len(raw):
        if offset + 7 > len(raw): raise ValueError('Short DSI packet header')
        header = raw[offset:offset + 7]
        length = int.from_bytes(header[5:7], 'big')
        end = offset + 7 + length
        if end > len(raw): raise ValueError('Short DSI packet payload')
        packets.append({'header': header.hex(' '), 'payload': raw[offset + 7:end].hex(' ')})
        offset = end
    return packets


panels = []
for relative in re.findall(r'#include\s+"([^"]+)"', OVERLAY.read_text()):
    path = BASE / relative
    source = path.read_bytes()
    text = clean(source.decode())
    modes, overrides = [], []
    for match in re.finditer(r'timing@([^\s{]+)\s*\{', text):
        node = block(text, match.end())
        mode = {'name': match[1]}
        for short, prop in [('hz', 'framerate'), ('width', 'width'), ('height', 'height')]:
            mode[short] = number(node, 'qcom,mdss-dsi-panel-' + prop)
        mode['timing_switch_packets'] = commands(node)
        if any(mode[key] is None for key in ['hz', 'width', 'height']):
            # Later &panel nodes can override commands without repeating geometry.
            # Keep them explicit; this extractor does not resolve a final live DT.
            overrides.append(mode)
            continue
        modes.append(mode)
    if not modes: raise ValueError(f'No timing nodes found: {relative}')
    panels.append({'source': str(path.relative_to(ROOT)),
        'sha256': hashlib.sha256(source).hexdigest(), 'modes': modes,
        'partial_timing_overrides': overrides})
output = ROOT / 'diagnostics/20261025-adreno120/dodge-panel-modes.json'
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps({'vendor_commit': subprocess.check_output(
    ['git', '-C', str(REPO), 'rev-parse', 'HEAD'], text=True).strip(),
    'connected_panel_identified': False, 'physical_refresh_measured': False,
    'panels': panels}, indent=2) + '\n')
for panel in panels:
    print(Path(panel['source']).name, [(m['width'], m['height'], m['hz']) for m in panel['modes']])
print(output)
