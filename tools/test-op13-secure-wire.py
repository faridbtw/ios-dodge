#!/usr/bin/env python3
"""Validate the vendor MP_ASSIGN layout, transition direction and address boundaries."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DEV = ROOT / 'source/xnu-build-snapshot/xnu-8796.121.2/bsd/dev'
source = r'''
#include <assert.h>
#include <stdlib.h>
#include "adreno8_secure_wire.h"
int main(void) {
    unsigned char *page = malloc(PAGE_SIZE);
    uint64_t r[4], expected[4] = {0x95e800040ULL,16,0x95e800000ULL,0x95e8000c0ULL};
    assert(a8_secure_assign_one(page,PAGE_SIZE,0x95e800000ULL,0x95e900000ULL,PAGE_SIZE,3,10,6,r));
    assert(!memcmp(r,expected,sizeof r));
    uint32_t source; memcpy(&source,page,4); assert(source==3);
    struct a8_secure_region region; memcpy(&region,page+64,sizeof region);
    assert(region.address==0x95e900000ULL && region.size==PAGE_SIZE);
    struct a8_secure_permission dest; memcpy(&dest,page+128,sizeof dest);
    assert(dest.vmid==10 && dest.permission==6 && !dest.context && !dest.context_size && !dest.reserved);
    uint64_t extra[7]; memcpy(extra,page+192,sizeof extra);
    assert(extra[0]==4 && extra[1]==0x95e800080ULL && extra[2]==24);
    for(unsigned i=3;i<7;i++) assert(extra[i]==0);
    assert(a8_secure_assign_one(page,PAGE_SIZE,0x95e800000ULL,0x95e900000ULL,PAGE_SIZE,10,3,7,r));
    memcpy(&source,page,4); memcpy(&dest,page+128,sizeof dest);
    assert(source==10 && dest.vmid==3 && dest.permission==7);
    memset(page,0x5a,PAGE_SIZE); memset(r,0xa5,sizeof r);
    assert(!a8_secure_assign_one(page,PAGE_SIZE,0x95e800000ULL,0x95e800000ULL,PAGE_SIZE,3,10,6,r));
    assert(!a8_secure_assign_one(page,PAGE_SIZE,0x95e800000ULL,0x95e800000ULL+PAGE_SIZE-4096,8192,3,10,6,r));
    assert(!a8_secure_assign_one(page,PAGE_SIZE,0x95e800001ULL,0x95e900000ULL,PAGE_SIZE,3,10,6,r));
    assert(!a8_secure_assign_one(page,PAGE_SIZE,0x95e800000ULL,(1ULL<<36)-4096,8192,3,10,6,r));
    assert(!a8_secure_assign_one(page,PAGE_SIZE,0x95e800000ULL,0x95e900000ULL,0,3,10,6,r));
    assert(!a8_secure_assign_one(page,PAGE_SIZE,0x95e800000ULL,0x95e900000ULL,PAGE_SIZE,3,10,7,r));
    assert(!a8_secure_assign_one(page,PAGE_SIZE,0x95e800000ULL,0x95e900000ULL,PAGE_SIZE,10,3,6,r));
    assert(!a8_secure_assign_one(page,PAGE_SIZE,0x95e800000ULL,0x95e900000ULL,PAGE_SIZE,3,9,6,r));
    assert(!a8_secure_assign_one(page,256,0x95e800000ULL,0x95e900000ULL,PAGE_SIZE,3,10,6,r));
    for(unsigned i=0;i<PAGE_SIZE;i++) assert(page[i]==0x5a);
    for(unsigned i=0;i<4;i++) assert(r[i]==0xa5a5a5a5a5a5a5a5ULL);
    struct a8_secure_region regions[128];
    for(unsigned i=0;i<128;i++) regions[i]=(struct a8_secure_region){0x960000000ULL+i*0x8000ULL,0x4000};
    assert(a8_secure_assign_regions(page,PAGE_SIZE,0x95e800000ULL,regions,78,false,r));
    assert(r[0]==0x95e800040ULL && r[1]==78*16 && r[2]==0x95e800000ULL && r[3]==0x95e800880ULL);
    memcpy(&dest,page+2112,sizeof dest); assert(dest.vmid==10 && dest.permission==6);
    memcpy(extra,page+2176,sizeof extra); assert(extra[0]==4 && extra[1]==0x95e800840ULL && extra[2]==24);
    for(unsigned i=0;i<78;i++) { memcpy(&region,page+64+i*16,sizeof region); assert(!memcmp(&region,&regions[i],16)); }
    assert(a8_secure_assign_regions(page,PAGE_SIZE,0x95e800000ULL,regions,128,true,r));
    memcpy(&dest,page+2112,sizeof dest); assert(dest.vmid==3 && dest.permission==6);
    memcpy(&dest,page+2136,sizeof dest); assert(dest.vmid==10 && dest.permission==4);
    memcpy(extra,page+2176,sizeof extra); assert(extra[2]==48);
    memset(page,0x5a,PAGE_SIZE); memset(r,0xa5,sizeof r);
    assert(!a8_secure_assign_regions(page,PAGE_SIZE,0x95e800000ULL,regions,129,true,r));
    assert(!a8_secure_assign_regions(page,PAGE_SIZE,0x95e800000ULL,regions,0,true,r));
    regions[1]=regions[0];
    assert(!a8_secure_assign_regions(page,PAGE_SIZE,0x95e800000ULL,regions,2,true,r));
    regions[0].address=0x95e800000ULL;
    assert(!a8_secure_assign_regions(page,PAGE_SIZE,0x95e800000ULL,regions,1,true,r));
    for(unsigned i=0;i<PAGE_SIZE;i++) assert(page[i]==0x5a);
    for(unsigned i=0;i<4;i++) assert(r[i]==0xa5a5a5a5a5a5a5a5ULL);
    free(page);
}
'''
with tempfile.TemporaryDirectory(prefix='op13-secure-wire-') as directory:
    p = Path(directory) / 'check.c'; p.write_text(source)
    for page in [4096, 16384]:
        binary = Path(directory) / f'check-{page}'
        subprocess.run(['cc','-std=c11',f'-DPAGE_SIZE={page}','-I',str(DEV),
            '-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
        print(f'PAGE_SIZE={page}: MP_ASSIGN layout, >4GiB addresses, permissions and boundary rejection: PASS')
