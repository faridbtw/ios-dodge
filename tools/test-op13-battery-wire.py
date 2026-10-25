#!/usr/bin/env python3
"""Check the PMIC get-only ABI against vendor enums and malformed wire data."""
from pathlib import Path
import re
import subprocess
import tempfile

r = Path(__file__).resolve().parents[1]
v = (r / 'diagnostics/20261025-battery/oplus_hal_adsp.h').read_text()
body = re.search(r'enum battery_property_id\s*\{(.*?)#ifdef', v, re.S)[1]
names = re.findall(r'\bBATT_[A-Z_]+\b', body)
expected = {'BATT_STATUS': 0, 'BATT_PRESENT': 2, 'BATT_CAPACITY': 4,
            'BATT_VOLT_NOW': 7, 'BATT_CURR_NOW': 9, 'BATT_TEMP': 12,
            'BATT_CHG_COUNTER': 14, 'BATT_CYCLE_COUNT': 15,
            'BATT_CHG_FULL_DESIGN': 16, 'BATT_CHG_FULL': 17}
assert all(names.index(k) == n for k, n in expected.items())
usb_body = re.search(r'enum usb_property_id\s*\{(.*?)\};', v, re.S)[1]
usb_names = re.findall(r'^\s*(USB_[A-Z0-9_]+)\s*,', usb_body, re.M)
assert usb_names.index('USB_IN_STATUS') == 50
assert re.search(r'#define MAX_OEM_PROPERTY_DATA_SIZE\s+128\b', v)
code = r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "op13_battery_wire.h"
int main(void) {
 struct op13_bc_message q, reply;
 const uint32_t battery[]={0,2,4,7,9,12,14,15,16,17}, usb[]={0,1,3,6,50};
 for(unsigned i=0;i<sizeof(battery)/4;i++) {
  assert(op13_bc_make_get(&q,0x30,battery[i]));
  assert(q.owner==32778 && q.type==1 && q.opcode==0x30);
  assert(!q.words[0] && q.words[1]==battery[i] && !q.words[2]);
 }
 for(unsigned i=0;i<sizeof(usb)/4;i++)assert(op13_bc_make_get(&q,0x32,usb[i]));
 assert(!op13_bc_make_get(&q,0x31,4)); /* Never battery SET. */
 assert(!op13_bc_make_get(&q,0x33,0)); /* Never charger SET. */
 assert(!op13_bc_make_get(&q,0x30,0xFFFFFFFF));
 assert(!op13_bc_make_get(NULL,0x30,4));
 reply=(struct op13_bc_message){32778,1,0x30,{4,10000,0}};
 assert(op13_bc_reply_valid(&reply,24));
 for(unsigned n=0;n<40;n++)if(n!=24)assert(!op13_bc_reply_valid(&reply,n));
 assert(!op13_bc_reply_valid(NULL,24));
 reply.owner++; assert(!op13_bc_reply_valid(&reply,24));reply.owner--;
 reply.type=2;assert(!op13_bc_reply_valid(&reply,24));reply.type=1;
 reply.opcode=0x31;assert(!op13_bc_reply_valid(&reply,24));reply.opcode=0x30;
 reply.words[0]=18;assert(!op13_bc_reply_valid(&reply,24));
 assert(op13_bc_sample_valid(10000,1,4,4465000,-261000,2960,1));
 assert(op13_bc_sample_valid(5000,1,2,8930000,-1000000,-500,0));
 assert(op13_bc_sample_valid(0,1,2,3000000,-1000000,1000,0));
 assert(!op13_bc_sample_valid(10001,1,1,4000000,0,2000,1));
 assert(!op13_bc_sample_valid(10000,0,1,4000000,0,2000,1));
 assert(!op13_bc_sample_valid(10000,1,5,4000000,0,2000,1));
 assert(!op13_bc_sample_valid(10000,1,1,4000000,0,2000,2));
 assert(!op13_bc_sample_valid(10000,1,1,13000000,0,2000,1));
 assert(!op13_bc_sample_valid(10000,1,1,4000000,INT32_MIN,2000,1));
 assert(!op13_bc_sample_valid(10000,1,1,4000000,0,INT32_MAX,1));
 struct op13_bc_oem_reply oem={.owner=32778,.type=1,.opcode=0x10000,.size=512};
 assert(op13_bc_oem_valid(&oem,528));
 for(unsigned n=0;n<550;n++)if(n!=528)assert(!op13_bc_oem_valid(&oem,n));
 for(unsigned n=0;n<550;n++) {
  oem.size=n;
  assert(op13_bc_oem_valid(&oem,528)==(n>=40 && n<=512 && !(n&3)));
 }
 oem.size=512;oem.owner++;assert(!op13_bc_oem_valid(&oem,528));oem.owner--;
 oem.type=2;assert(!op13_bc_oem_valid(&oem,528));oem.type=1;
 oem.opcode=0x10001;assert(!op13_bc_oem_valid(&oem,528));
 assert(!op13_bc_oem_valid(NULL,528));
 puts("Vendor IDs, get-only requests, malformed replies and sample boundaries passed");
}
'''
with tempfile.TemporaryDirectory() as d:
    c = Path(d) / 'wire.c'; c.write_text(code)
    exe = Path(d) / 'wire'
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(r / 'source/xnu-build-snapshot/xnu-8796.121.2/bsd/dev'),
                    str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
