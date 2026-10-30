# Current source: Project Boot v25 Wi-Fi handoff

Historical directory name retained for existing build tools. Current source
includes the Project Boot menu, centered logo/title, boxed touch-scrollable logs
above the logo, persistent Logs label, USB reconnect and seven-core live reset.
The exclusive native Wi-Fi MHI handoff uses a separately reserved 128 MiB DMA pool.

Validated local UEFI SHA-256:
2ee6c2a0def66dcb9ebe7a33268d3a1372202f1501de7219083fb647ab760955

Build with RAM_ONLY=1 LOADER_PLATFORM=dodge EMBEDDED_USB=1 and LLVM in PATH.
Local templates/images and private native firmware are not published.
