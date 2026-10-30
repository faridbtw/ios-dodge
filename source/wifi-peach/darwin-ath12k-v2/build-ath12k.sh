#!/bin/bash
set -e
ROOT=$(cd "$(dirname "$0")" && pwd)
B=$ROOT
cd "$B"
W=$ROOT/wifi/lx

FREEBSD=15.1
FREEBSD_SHA256=cf5762da53fd52e1eaf0f9ceee9bf58cbe314c821031d0d9ffa76823185a89e1
FSYS="compat/linuxkpi sys arm64/include net net80211 netinet netinet6 vm kern tools libkern ddb
	security fs/pseudofs crypto opencrypto geom bsm isa cam rpc xdr x86/include ofed/include
	dev/pci dev/ofw dev/fdt dev/acpica dev/usb dev/iicbus dev/backlight
	contrib/ck/include contrib/dev/acpica/include contrib/libnv"
PATCHES=$(cat "$W"/patches/*.patch "$ROOT"/wifi/lx12/fixup.py 2>/dev/null | sha256sum | cut -d' ' -f1)
if [ "$(cat freebsd/.release 2>/dev/null)" != "$FREEBSD $FREEBSD_SHA256 $PATCHES" ]; then
	[ -f freebsd-$FREEBSD-src.txz ] || [ ! -f $A11/freebsd-$FREEBSD-src.txz ] || ln -sf $A11/freebsd-$FREEBSD-src.txz .
	[ -f freebsd-$FREEBSD-src.txz ] || curl -sL --fail -o freebsd-$FREEBSD-src.txz \
		https://download.freebsd.org/releases/amd64/$FREEBSD-RELEASE/src.txz
	echo "$FREEBSD_SHA256  freebsd-$FREEBSD-src.txz" | sha256sum -c --quiet
	rm -rf freebsd gen obj
	mkdir freebsd
	tar xJf freebsd-$FREEBSD-src.txz -C freebsd --strip-components=2 usr/src/COPYRIGHT \
		$(for d in $FSYS; do echo usr/src/sys/$d; done)
	for p in "$W"/patches/*.patch; do
		[ -f "$p" ] && patch -s --no-backup-if-mismatch -d freebsd -p1 < "$p"
	done
	echo "$FREEBSD $FREEBSD_SHA256 $PATCHES" > freebsd/.release
fi
python3 "$ROOT"/wifi/lx12/fixup.py "$B/freebsd/sys" "$B/src/ath12k" "$B/linuxref/include"
S=$B/freebsd/sys

if [ ! -d gen ]; then
	mkdir gen
	(
		cd gen
		ln -s $S/arm64/include machine
		for m in kern/device_if.m kern/bus_if.m dev/pci/pci_if.m dev/pci/pci_iov_if.m dev/pci/pcib_if.m \
			dev/iicbus/iicbus_if.m dev/iicbus/iicbb_if.m dev/backlight/backlight_if.m dev/usb/usb_if.m \
			dev/ofw/ofw_bus_if.m; do
			awk -f $S/tools/makeobjops.awk $S/$m -h
		done
		for o in h p q; do awk -f $S/tools/vnode_if.awk $S/kern/vnode_if.src -$o; done
		for o in $(grep -rhoE "opt_[a-z0-9_]+\.h" $S | sort -u) lkpi_iic_if.h; do : > $o; done
		printf '#define INET 1\n#define INET6 1\n' > opt_inet.h
		printf '#define INET6 1\n' > opt_inet6.h
		printf '#define SMP 1\n' > opt_global.h
	)
fi

DRIVER="core hal wmi mac reg htc qmi dp dp_tx dp_rx dp_htt dp_peer debug ce peer dbring dp_mon fw p2p
	wifi7/core wifi7/hw wifi7/hal wifi7/hal_qcc2072 wifi7/hal_wcn7850 wifi7/hal_qcn9274 wifi7/hal_rx wifi7/hal_tx
	wifi7/dp wifi7/dp_rx wifi7/dp_tx wifi7/dp_mon wifi7/ce wifi7/wmi"
CC="clang -target arm64-apple-ios16.5"
LD=/usr/bin/ld
CFLAGS="-mkernel -mgeneral-regs-only -fno-builtin -fno-common -ffreestanding -fno-strict-aliasing
	-funsigned-char -nostdinc -std=gnu17 -O2 -g -w
	-D__FreeBSD__=15 -D_KERNEL -DKLD_MODULE -DHAVE_KERNEL_OPTION_HEADERS -include opt_global.h
	-I$ROOT/wifi/lx12/include -I$W/include -Igen -I$S -I$S/contrib/ck/include
	-I$S/compat/linuxkpi/common/include -I$S/compat/linuxkpi/dummy/include
	-include $S/compat/linuxkpi/common/include/linux/kconfig.h -DLINUXKPI_VERSION=70000"
DFLAGS="-Isrc/ath12k -Isrc -include $W/include/lx_prelude.h -include lx12_prelude.h -DCONFIG_ATH12K_DEBUG
	-DKBUILD_MODNAME=\"ath12k\""
XNU_SRC=$ROOT/kernel-abi
for h in lx qmi; do
	[ -f "$XNU_SRC/bsd/sys/macan_$h.h" ] || { echo "mk-ath12k.sh: the kernel tree has no bsd/sys/macan_$h.h" >&2; exit 1; }
done
DFLAGS="$DFLAGS -DLX_REDWOOD_LX_H=\"$XNU_SRC/bsd/sys/macan_lx.h\" -DLX_REDWOOD_QMI_H=\"$XNU_SRC/bsd/sys/macan_qmi.h\""
mkdir -p obj out
rm -f obj/*.err
# Dependency files contain absolute include paths. A copied checkout must not
# reuse objects compiled against its source directory of origin.
BUILD_CONTEXT=$(printf '%s\n' "$ROOT" "$CC" "$CFLAGS" "$DFLAGS" | sha256sum | cut -d' ' -f1)
CACHE_VALID=0
[ "$(cat obj/.build-context 2>/dev/null)" != "$BUILD_CONTEXT" ] || CACHE_VALID=1
compile() {
	local src=$1 obj=$2
	shift 2
	if [ "$CACHE_VALID" = 1 ] && [ -f "$obj" ] && [ "$obj" -nt "$src" ] && [ -f "$obj.d" ] && [ -z "$(find $(sed -e 's/^[^:]*://' -e 's/\\$//' "$obj.d") "$0" -newer "$obj" 2>/dev/null | head -1)" ]; then
		return
	fi
	$CC $CFLAGS "$@" -MMD -MF "$obj.d" -c "$src" -o "$obj" 2> "$obj.err" || rm -f "$obj"
}
LXSRC="$W/src/lx_base.c $W/src/lx_80211.c $W/src/lx_wpa.c $ROOT/wifi/lx12/src/lx12_bus.c $ROOT/wifi/lx12/src/lx12_glue.c"
for f in $DRIVER; do compile src/ath12k/$f.c obj/ath12k_$(echo $f | tr / _).o $DFLAGS & done
for f in $LXSRC; do compile "$f" obj/lx_$(basename "$f" .c).o $DFLAGS -I$W/src & done
wait
bad=
for f in obj/*.err; do
	[ -s "$f" ] && [ ! -f "${f%.err}" ] && { echo "--- $(basename "$f" .o.err)"; grep -m6 -A3 "error:" "$f"; bad=1; }
done
[ -z "$bad" ] || exit 1
printf '%s\n' "$BUILD_CONTEXT" > obj/.build-context
OBJECTS=$(for f in $DRIVER; do echo obj/ath12k_$(echo $f | tr / _).o; done; for f in $LXSRC; do echo obj/lx_$(basename "$f" .c).o; done)

KERNEL="memcmp memcpy memmove memset strcmp strncmp strlen strchr strrchr strlcpy strlcat bzero
	snprintf vsnprintf __stack_chk_fail __stack_chk_guard"
$LD -r -arch arm64 $OBJECTS -o out/all.o 2> out/ld.err || { cat out/ld.err; exit 1; }
llvm-nm -u --format=just-symbols out/all.o | sed 's/^_//' | sort > out/undefined.txt
: > out/stubs.txt
: > out/stubs.s
missing=
for s in $(cat out/undefined.txt); do
	case " $(echo $KERNEL) " in *" $s "*) continue ;; esac
	case $s in
	lx_os_*|qmi_*|redwood_qmi_*|section\$*) continue ;;
	ath12k_*|__ath12k_*) ;;
	*) echo "mk-ath12k.sh: nothing defines $s" >&2; missing=1; continue ;;
	esac
	echo $s >> out/stubs.txt
	printf '\t.globl _%s\n_%s:\n\tadrp x0, 1f@PAGE\n\tadd x0, x0, 1f@PAGEOFF\n\tb _lx_stub\n\t.cstring\n1:\t.asciz "%s"\n\t.text\n\t.p2align 2\n' \
		$s $s $s >> out/stubs.s
done
[ -z "$missing" ] || exit 1
$CC -c out/stubs.s -o out/stubs.o
printf '_lx_prepare\n_lx_start\n_lx_stop\n_lx_transmit\n' > out/exported.txt
$LD -r -arch arm64 -exported_symbols_list out/exported.txt out/all.o out/stubs.o -o out/ath12k.o 2> out/ld.err ||
	{ cat out/ld.err; exit 1; }
rm -f out/all.o
echo "mk-ath12k.sh: out/ath12k.o: $(llvm-size out/ath12k.o | awk 'NR == 2 { print $1 + $2 }') bytes of code and data from" \
	"$(echo $DRIVER | wc -w) of the driver's files; $(wc -l < out/stubs.txt) functions of the others are stubs (out/stubs.txt);" \
	"asks the kernel for $(llvm-nm -u --format=just-symbols out/ath12k.o | grep -vc 'section\$') names"
