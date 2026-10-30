# Peach PCIe / Darwin Wi-Fi

Device-verified on OnePlus 13 (2026-10-30): Settings scan/join, WPA2/CCMP,
IPv4, DNS and HTTPS. User confirmed Wi-Fi off/on and Safari. See
[working build and adaptation notes](../WORKING.md) and UPSTREAM.json.

This is the actual Peach 17cb:110e PCIe profile, adapted from ios-macan's
LinuxKPI/ath12k station stack. MHI/QRTR ownership and DMA are provided by
bsd/dev/op13wifi in the published XNU snapshot. Three unused upstream PCI/MHI
symbols remain stubs; they are not the active transport.

Build: PATH=/opt/homebrew/opt/llvm/bin:$PATH bash build-ath12k.sh
Requires the pinned FreeBSD 15.1 archive (automatically fetched/verified) and
LLVM. Linux 7.0.14 ath12k/ABI sources and license notices are included. Build
outputs, dependencies and private device inputs are not committed. Native
revisions, archive hashes, descriptor/register and wake audits are recorded here.
