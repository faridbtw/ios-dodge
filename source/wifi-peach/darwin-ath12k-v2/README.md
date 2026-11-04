# Experimental Peach PCIe adaptation (v2)

This fork now includes an explicit Peach hardware profile, a PCIe bus ABI,
and an already-booted-firmware QMI path. It is used only with the isolated
Darwin v2 test kernel and its validated 128 MiB handoff. It is not selected
for the production boot. Network operation is not yet verified. The three
unused upstream PCI/MHI stubs remain unused; the real MHI transport lives
in the Darwin kernel. See experimental/wifi/darwin-peach-v2/README.md.

The baseline import notes below describe the original v1 state.

# OnePlus13 Darwin ath12k import

Imported the complete ios-macan LinuxKPI, station/WPA2 layer and Linux7.0.14
ath12k sources. FreeBSD15.1 headers and Linux archives verified against exact
upstream pins; source notices and licenses retained. build-ath12k.sh builds a
Mach-O ARM64 iOS16.5 driver object natively on this Mac. No iOS image or phone
storage is changed by this build.

The resulting out/ath12k.o is a REFERENCE BUILD BASELINE. It still contains
macan's WPSS bus and hardware inputs, and MUST NOT be started on OnePlus13.
Three upstream PCI/MHI-only references are stubs exactly as in the reference
build (out/stubs.txt); they do not implement PCIe/MHI. Actual runtime port
must replace that bus with Peach PCIe/MHI and owned DMA/SMMU transport, not
rename a QCC2072 device ID. Local stock driver identifies17cb:110e as Peach;
QCC2072/Cologne is17cb:1112, WCN7750 is a separate WPSS device.

peach-qcc2072-descriptor-audit.json compares eight critical stock Peachv2 vs
QCC2072v1 hardware headers: 2312 numeric descriptor definitions match exactly,
without mismatches or additional fields in those headers. This is evidence
for reusing the packet layout/ath12k datapath; it is not proof of identical
bus, clocks, firmware or all hardware registers.

Build: PATH=/opt/homebrew/opt/llvm/bin:$PATH bash build-ath12k.sh
Host WPA2 test: test-wpa passed standard PBKDF2 vector and10000 malformed
EAPOL/truncation checks under ASAN/UBSAN. The driver object compiled33 source
units,593856 bytes code+data,47 kernel imports. See UPSTREAM.json for hashes.

Current hardware experiments are separate: experimental/wifi/radio-start-v20
(native radio start, scan lifecycle but zero APframes), htt-version-v21
(real CE4/CE1 replies, rejected optional version TLVs), htt-options-v22
(corrected version-first-word parser; test pending). Wi-Fi is NOT working;
no network-interface, association, DHCP or Internet success is claimed.
