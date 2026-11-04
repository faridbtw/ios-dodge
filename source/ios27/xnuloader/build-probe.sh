#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export PATH="${LLVM19_BIN:-/usr/lib/llvm-19/bin}:$PATH"
export RAM_ONLY=1 LOADER_PLATFORM=dodge LOADER_BASE=0xC4000000
export USB_IDLE_SECONDS=0 EMBEDDED_USB=1
exec bash build.sh
