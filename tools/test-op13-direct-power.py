#!/usr/bin/env python3
"""Exercise the production alternative power probe and its failure boundaries."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
dev = root / 'source/xnu-build-snapshot/xnu-8796.121.2/bsd/dev'
source = (dev / 'adreno8_hfi.c').read_text()
start = source.index('bool\nadreno8_direct_power_probe(void)')
end = source.index('\n/* ---- step 16:', start)
constants = '\n'.join(line for line in (dev / 'adreno8_private.h').read_text().splitlines()
                      if line.startswith('#define GMU'))
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
struct block { uintptr_t base; } adreno8_gmu = {0x100000}, adreno8_gpu = {0x200000};
bool adreno8_gpu8_keep, adreno8_gpu8_return, adreno8_gmu_talks;
bool adreno8_hfi_tables_sent, adreno8_reset_required;
static bool adreno8_direct_power_on;
uint32_t adreno8_gx_count, adreno8_gx_hz[17];
static unsigned enabled, calls, writes, reads, fail_at;
static bool cx, scheduler, ack, power_up;
bool PE_parse_boot_argn(const char *key, void *value, unsigned long size) {
    assert(!strcmp(key,"a8_direct") && size==4); *(uint32_t *)value=enabled;return true;
}
bool adreno8_cx_ready(void) {return cx;}
bool adreno8_hwsched_ready(void) {return scheduler;}
void adreno8_announce(const char *s, ...) {(void)s;}
void adreno8_sleep_us(uint32_t us) {(void)us;}
void adreno8_settle(void) {}
bool adreno8_hfi_send(uint32_t id, const uint32_t *p, unsigned count, const char *name) {
    (void)name;
    const uint32_t ids[]={11,11,14,30}, sizes[]={3,3,1,3};
    const uint32_t payloads[][3]={{1,0,0},{9,0,0},{0,0,0},{1,1,2}};
    assert(calls<4 && id==ids[calls] && count==sizes[calls]);
    assert(!memcmp(p,payloads[calls],count*4));calls++;return calls!=fail_at;
}
''' + constants + r'''
void adreno8_wr(uintptr_t address, uint32_t value) {
    assert(calls==4);
    if(writes==0) assert(address==adreno8_gmu.base+GMU_HOST2GMU_SET && value==(1u<<30));
    else assert(writes==1 && address==adreno8_gmu.base+GMU_GMU2HOST_CLR && value==(1u<<31));
    writes++;
}
uint32_t adreno8_rd(uintptr_t address) {
    assert(writes==1);
    if(address==adreno8_gmu.base+GMU_GMU2HOST_INFO)return ack?1u<<31:0;
    assert(address==adreno8_gmu.base+GMU_PWR_CLK_STATUS);return power_up?0:3;
}
uint32_t adreno8_get(struct block *b,const char *name,uint32_t offset) {
    (void)name;assert(b==&adreno8_gpu && offset==0x48 && writes==2);reads++;return 0;
}
''' + source[start:end] + r'''
static void reset(void) {
    enabled=1;calls=writes=reads=fail_at=0;cx=ack=power_up=true;scheduler=adreno8_direct_power_on=false;
    adreno8_gpu8_keep=adreno8_gmu_talks=adreno8_hfi_tables_sent=true;
    adreno8_gpu8_return=adreno8_reset_required=false;
    adreno8_gx_count=2;adreno8_gx_hz[1]=125000000;
}
int main(void) {
    for(unsigned i=0;i<10;i++) {
        reset();
        switch(i) {
        case 0:enabled=0;break;case 1:enabled=3;break;case 2:cx=false;break;
        case 3:adreno8_gpu8_keep=false;break;case 4:adreno8_gpu8_return=true;break;
        case 5:adreno8_gmu_talks=false;break;case 6:adreno8_hfi_tables_sent=false;break;
        case 7:scheduler=true;break;case 8:adreno8_gx_count=3;break;
        case 9:adreno8_gx_hz[1]=195000000;break;
        }
        assert(!adreno8_direct_power_probe() && !calls && !writes && !adreno8_reset_required);
    }
    for(unsigned i=1;i<=4;i++) {
        reset();fail_at=i;assert(!adreno8_direct_power_probe());
        assert(calls==i && !writes && !reads && adreno8_reset_required);
    }
    reset();ack=false;assert(!adreno8_direct_power_probe() && writes==1 && !reads && adreno8_reset_required);
    reset();power_up=false;assert(!adreno8_direct_power_probe() && writes==1 && !reads && adreno8_reset_required);
    reset();assert(adreno8_direct_power_probe() && calls==4 && writes==2 && reads==1 && adreno8_reset_required);
    reset();enabled=2;assert(adreno8_direct_power_probe() && adreno8_direct_power_ready());
    puts("18 direct-power startup/failure scenarios passed; no CP register writes");
}
'''
with tempfile.TemporaryDirectory() as d:
    c = Path(d) / 'probe.c'
    c.write_text(harness)
    binary = Path(d) / 'probe'
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
