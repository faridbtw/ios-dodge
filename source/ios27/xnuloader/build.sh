#!/bin/bash
# Build the XNU loader as a freestanding AArch64 UEFI application.
set -e
cd "$(dirname "$0")"
defs=()
[ "${EMBEDDED_USB:-0}" = 1 ] && defs+=(-DXNULOADER_EMBEDDED_USB=1)
[ -n "${USB_IDLE_SECONDS:-}" ] && defs+=("-DUSB_IDLE_SECONDS=${USB_IDLE_SECONDS}")
[ -n "${PAYLOAD_BASE:-}" ] && defs+=("-DPAYLOAD_BASE=${PAYLOAD_BASE}ULL")
[ "${RAM_ONLY:-0}" = 1 ] && defs+=(-DRAM_ONLY=1)
[ "${LOADER_PLATFORM:-}" = dodge ] && defs+=(-DXNULOADER_PLATFORM_DODGE=1)
clang "${defs[@]}" --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -Oz -Wall -Wextra -Wno-unused-parameter -I. -c loader.c -o loader.obj
# /base: UEFI loads an image at its preferred address when that is free. The PE default (0x140000000)
# is in the middle of the phone's largest block of RAM, which XNU needs in one piece.
for module in s3910 op13_bitbang fifo_snapshot geni_preflight sm8750_prepare sm8750_spi_inspect sm8750_config_query sm8750_first_read sm8750_display_query loader/sm8750_power loader/sm8750_avdd panel-prep/sm8750_panel panel-prep/sm8750_panel_raw; do
 clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar -mgeneral-regs-only -O1 -Wall -Wextra -I. -c "$module.c" -o "$module.obj"
done
clang --target=aarch64-unknown-windows -DOP13_MHI_UEFI -DOP13_MHI_RDDM_PREARM -ffreestanding -fno-builtin -fno-stack-protector -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter -I. -c mhi_host.c -o mhi_host.obj
clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -mgeneral-regs-only -O1 -Wall -Wextra -I. -c mhi_uefi_compat.c -o mhi_uefi_compat.obj
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:${LOADER_BASE:-0xA1000000} /out:loader.efi mhi_host.obj mhi_uefi_compat.obj s3910.obj op13_bitbang.obj fifo_snapshot.obj geni_preflight.obj loader.obj sm8750_prepare.obj sm8750_spi_inspect.obj sm8750_config_query.obj sm8750_first_read.obj sm8750_display_query.obj loader/sm8750_power.obj loader/sm8750_avdd.obj panel-prep/sm8750_panel.obj panel-prep/sm8750_panel_raw.obj
ls -la loader.efi
