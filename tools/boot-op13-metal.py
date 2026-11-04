#!/usr/bin/env python3
"""RAM-only iOS Metal boot with the device-verified Peach Wi-Fi initialization."""
from pathlib import Path
import runpy

if __name__ == '__main__':
    root=Path(__file__).resolve().parents[1]
    runpy.run_path(str(root/'experimental/gpu-dada/v1/boot-metal-ui.py'),run_name='__main__')
