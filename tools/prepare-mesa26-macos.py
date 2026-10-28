#!/usr/bin/env python3
"""Compatibility entry point for the verified, consolidated GLES Mesa source."""
from pathlib import Path
import runpy

runpy.run_path(str(Path(__file__).with_name('prepare-op13-gles-mesa.py')), run_name='__main__')
