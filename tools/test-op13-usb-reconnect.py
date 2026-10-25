#!/usr/bin/env python3
"""Exercise the actual loader transport with a simulated Qualcomm USB protocol."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'source/touch-runtime-v15'
loader = (SOURCE / 'loader.c').read_text()
transport = loader[loader.index('/* All synchronous and asynchronous receive paths share'):loader.index('\nstatic void\nfb_reply(')]
prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "efi.h"
#define USB_EP_OUT 1
#define USB_EP_IN 0x81
#define USB_CHUNK (1024 * 1024)
#define USB_IDLE_SECONDS 0
static uint64_t ticks;
#define READ_SYSREG(reg) test_##reg()
static uint64_t test_CNTVCT_EL0(void) { return ++ticks; }
static uint64_t test_CNTFRQ_EL0(void) { return 1000; }
static void *running_live;
static int touch_diagnostic_retained(void) { return 0; }
static void print(const char *fmt, ...) { (void)fmt; }
static void reboot(void) { abort(); }
static void wdt_pet(void) {}
static uint64_t usb_last_activity, usb_disconnects, usb_connects;
static int usb_connected, usb_rx_queued, usb_out_pending, usb_in_pending;
static int download_to_ramdisk;
static char usb_session[17] = "unit-test";
static uint8_t rx[USB_CHUNK], tx[USB_CHUNK];
static uint8_t *usb_tx_buf = tx;
static usb_device_protocol protocol, *USB = &protocol;
struct event { uint32_t type; usb_device_event_data data; };
static struct event events[128];
static unsigned head, tail, sends, aborts;
static int driver_out_busy, driver_in_busy;
static int fail_send, disconnect_send, active_send, no_completion;
static void link_event(uint32_t state) {
    assert(tail < 128); events[tail].type = UsbDeviceEventDeviceStateChange;
    events[tail++].data.device_state = state;
}
static void transfer_event(uint8_t ep, uint32_t status, uint64_t bytes) {
    assert(tail < 128); events[tail].type = UsbDeviceEventTransferNotification;
    events[tail++].data.transfer = (typeof(events[0].data.transfer)){status, ep, bytes, rx};
}
static efi_status mock_send(uint8_t ep, uint64_t bytes, void *buffer) {
    (void)buffer; sends++;
    int *busy = ep == USB_EP_OUT ? &driver_out_busy : &driver_in_busy;
    if (fail_send) { fail_send = 0; link_event(UsbDeviceStateDisconnected);
        link_event(UsbDeviceStateConnected); return EFI_ERROR_BIT | 2; }
    if (*busy) return EFI_ERROR_BIT | 2;
    *busy = 1;
    if (disconnect_send) { disconnect_send = 0; link_event(UsbDeviceStateDisconnected); }
    else if (!no_completion) {
        if (active_send) { active_send = 0; transfer_event(ep, UsbDeviceTransferStatusActive, 0); }
        transfer_event(ep, UsbDeviceTransferStatusCompleteOK, bytes);
    }
    return 0;
}
static efi_status mock_abort(uint8_t ep) {
    aborts++;
    if (ep == USB_EP_OUT) driver_out_busy = 0; else driver_in_busy = 0;
    transfer_event(ep, UsbDeviceTransferStatusCancelled, 0);
    return 0;
}
static efi_status mock_handle(uint32_t *type, uint64_t *size, usb_device_event_data *data) {
    *size = sizeof(*data);
    if (head == tail) { *type = UsbDeviceEventNoEvent; return 0; }
    struct event e = events[head++]; *type = e.type; *data = e.data;
    if (e.type == UsbDeviceEventTransferNotification &&
        e.data.transfer.status != UsbDeviceTransferStatusActive) {
        if (e.data.transfer.endpoint_index == USB_EP_OUT) driver_out_busy = 0;
        else driver_in_busy = 0;
    }
    return 0;
}
static void reset(void) {
    memset(&protocol, 0, sizeof(protocol));
    protocol.send = mock_send; protocol.abort_xfer = mock_abort; protocol.handle_event = mock_handle;
    head = tail = sends = aborts = 0; ticks = usb_last_activity = 0;
    usb_connected = 1; usb_rx_queued = usb_out_pending = usb_in_pending = 0;
    usb_connects = usb_disconnects = 0; download_to_ramdisk = 0; running_live = NULL;
    driver_out_busy = driver_in_busy = fail_send = disconnect_send = active_send = no_completion = 0;
}
'''
suffix = r'''
int main(void) {
    reset(); active_send = 1;
    assert(usb_rx(rx, 511) == 511); /* Active is not a terminal completion. */
    assert(!usb_out_pending);

    reset(); disconnect_send = 1; download_to_ramdisk = 1;
    assert(usb_rx(rx, 511) == -1);
    assert(!usb_connected && !usb_out_pending && !usb_rx_queued && !download_to_ramdisk);
    assert(aborts == 1); /* Disconnect aborts the posted receive. */
    link_event(UsbDeviceStateConnected);
    assert(usb_rx(rx, 511) == 511);
    assert(usb_connected && usb_connects == 1 && usb_disconnects == 1);

    reset(); fail_send = 1; driver_out_busy = 1;
    assert(usb_rx(rx, 511) == -1);
    assert(head == tail && !driver_out_busy && usb_connected);
    assert(usb_rx(rx, 511) == 511); /* A failed Send cannot starve link events. */

    reset(); usb_rx_queued = usb_out_pending = driver_out_busy = 1;
    transfer_event(USB_EP_OUT, UsbDeviceTransferStatusCancelled, 0);
    assert(usb_rx(rx, 511) == -1);
    assert(!usb_rx_queued && !usb_out_pending);
    assert(usb_rx(rx, 511) == 511);

    reset(); disconnect_send = 1;
    assert(usb_tx((const uint8_t *)"OKAY", 4) == -1);
    unsigned previous_sends = sends;
    assert(usb_tx((const uint8_t *)"OKAY", 4) == -1 && sends == previous_sends);
    link_event(UsbDeviceStateConnected);
    assert(usb_rx(rx, 511) == 511); assert(usb_tx((const uint8_t *)"OKAY", 4) == 4);

    reset(); running_live = (void *)1; no_completion = 1;
    assert(usb_rx(rx, 511) == -1); /* A bounded kernel-side wait must abort its buffer. */
    assert(aborts == 1 && !usb_out_pending);
    running_live = NULL; no_completion = 0;
    assert(usb_rx(rx, 511) == 511);

    reset();
    for (unsigned cycle = 0; cycle < 12; cycle++) {
        disconnect_send = 1;
        assert(usb_rx(rx, 511) == -1);
        link_event(UsbDeviceStateConnected);
        assert(usb_rx(rx, USB_CHUNK) == USB_CHUNK);
        assert(usb_tx(tx, 64) == 64);
        head = tail = 0;
    }
    assert(usb_disconnects == 12 && usb_connects == 12);
    puts("PASS: active/cancelled transfers, queued receives, failed-send event starvation, TX disconnect, kernel timeout, 12 reconnect cycles.");
}
'''
with tempfile.TemporaryDirectory(prefix='op13-usb-test-') as directory:
    directory = Path(directory)
    (directory / 'test.c').write_text(prefix + transport + suffix)
    subprocess.run(['/usr/bin/clang', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', '-I', str(SOURCE),
                    str(directory / 'test.c'), '-o', str(directory / 'test')], check=True)
    subprocess.run([str(directory / 'test')], check=True, timeout=30)
