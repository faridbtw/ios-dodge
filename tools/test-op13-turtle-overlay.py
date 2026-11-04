#!/usr/bin/env python3
"""Test the loader's real overlay/console integration without firmware hardware."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'experimental/uefi-turtle/stable-overlay-v11/loader-src'
loader = (SOURCE / 'loader.c').read_text()
integration = loader[loader.index('#include "op13_big_console.h"'):loader.index('/* Draw once while UEFI owns the display;')]
prefix = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
static struct { uint64_t base; uint32_t width, height, stride; } SCREEN;
static int screen_quiet;
static uint64_t fake_ticks;
#define READ_SYSREG(reg) test_##reg()
static uint64_t test_CNTVCT_EL0(void) { return fake_ticks += 100; }
static uint64_t test_CNTFRQ_EL0(void) { return 1000; }
static void clean_dcache(uint64_t lo, uint64_t hi) { assert(hi >= lo); }
'''
suffix = r'''
int main(void) {
    const unsigned width = 1440, height = 3168, stride = width * 4;
    const size_t bytes = (size_t)stride * height;
    unsigned char *allocation = malloc(bytes + 128), *reference = calloc(1, bytes), *shadow = calloc(1, bytes);
    assert(allocation && reference && shadow);
    memset(allocation, 0xa5, bytes + 128);
    unsigned char *pixels = allocation + 64;
    memset(pixels, 0, bytes);
    SCREEN.base = (uintptr_t)pixels; SCREEN.width = width;
    SCREEN.height = height; SCREEN.stride = stride;
    struct op13_big_console normal;
    console_shadow = (uintptr_t)shadow;
    big_console_ready = op13_console_init(&big_console, shadow, bytes, width, height, stride, 64, height - 32);
    assert(big_console_ready);
    assert(op13_console_init(&normal, reference, bytes, width, height, stride, 64, height - 32));
    console_dirty_top = 0; console_dirty_bottom = height;
    screen_log_flush(1);
    unsigned samples[32], count = 0;
    for (unsigned i = 0; i < 90000 && count < 32; i += 37)
        if (turtle_pixels[i] > 250) samples[count++] = i;
    assert(count > 10);
    for (unsigned line = 0; line < 95; line++) {
        const char *s = "abcdefghijklmnopqrstuvwxyz 0123456789: regular full-screen log text\n";
        while (*s) {
            assert(screen_log_char(*s));
            op13_console_char(&normal, (unsigned char)*s++);
            assert(big_console.row == normal.row && big_console.column == normal.column);
            /* The physical logo must stay visible even between characters/lines. */
            for (unsigned i = 0; i < count; i++) {
                unsigned y = samples[i] / 300, x = samples[i] % 300;
                assert((((unsigned *)pixels)[(1434 + y) * width + 570 + x] & 255) > 250);
            }
        }
        screen_log_flush(1);
        for (unsigned y = 0; y < 300; y++) for (unsigned x = 0; x < 300; x++)
            if (turtle_pixels[y * 300 + x] > 250)
                assert((((unsigned *)pixels)[(1434 + y) * width + 570 + x] & 255) > 250);
    }
    /* The text surface must match a normal console, including under the logo. */
    assert(memcmp(shadow, reference, bytes) == 0);
    for (unsigned i = 0; i < 64; i++) {
        assert(allocation[i] == 0xa5 && pixels[bytes + i] == 0xa5);
    }
    free(allocation); free(reference); free(shadow);
    puts("PASS: text surface matches full-screen console; physical turtle stays visible during every update and scroll; memory guards intact.");
}
'''
with tempfile.TemporaryDirectory(prefix='op13-overlay-test-') as directory:
    directory = Path(directory)
    (directory / 'test.c').write_text(prefix + integration + suffix)
    subprocess.run(['/usr/bin/clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', '-I', str(SOURCE),
                    str(directory / 'test.c'), '-o', str(directory / 'test')], check=True)
    subprocess.run([str(directory / 'test')], check=True, timeout=60)
