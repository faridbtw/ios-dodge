#!/usr/bin/env python3
"""Exercise the production bridge transaction against mocked SCM and memory failures."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
driver = ROOT / 'source/xnu-build-snapshot/xnu-8796.121.2/bsd/dev/adreno8_secure.c'
production = driver.read_text().replace('#include "adreno8_private.h"', '')
start = production.index('static struct a8_scm_result\nsecure_scm(')
end = production.index('\n}\n', start) + 3
provider = r'''
static struct a8_scm_result secure_scm(uint64_t fn, uint64_t info,
    uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    struct a8_scm_result r = {0};
    if (fn == SCM_AVAILABLE) {
        assert(info == 1 && a1 == 0 && a2 == 0 && a3 == 0);
        queries++; r.value[0] = 1;
        if (scenario == 1 && a0 == 0x02000c1e) r.value[0] = 0;
        if (scenario == 13 && a0 == 0x02000c16) r.value[0] = 0;
    } else if (fn == SCM_BRIDGE_ENABLE) {
        assert(info == 0 && !(a0 | a1 | a2 | a3)); enables++;
        if (scenario == 2) r.status = -5;
    } else if (fn == SCM_BRIDGE_CREATE) {
        assert(info == 4 && a0 == (fake_physical | 6) && a1 == (fake_physical | 6));
        assert(a2 == ((uint64_t)PAGE_SIZE | (1ULL << 9)) && a3 == 3);
        assert(publishes == 1); creates++;
        r.value[1] = 0x123456789abcdef0ULL;
        if (scenario == 4) r.value[0] = 15;
        if (scenario == 6) r.status = 2;
    } else if (fn == SCM_BRIDGE_DELETE) {
        assert(info == 1 && a0 == 0x123456789abcdef0ULL && !(a1 | a2 | a3));
        deletes++; if (scenario == 5) r.status = -5;
    } else if (fn == SCM_ASSIGN) {
        assert(info == 0x1117 && a0 == fake_physical+64 && a1 == 16);
        assert(a2 == fake_physical && a3 == fake_physical+192);
        uint32_t src; struct a8_secure_permission dest;
        struct a8_secure_region region; uint64_t ext[7];
        memcpy(&src,allocated[0],4); memcpy(&dest,(char*)allocated[0]+128,sizeof dest);
        memcpy(&region,(char*)allocated[0]+64,sizeof region);
        memcpy(ext,(char*)allocated[0]+192,sizeof ext);
        assert(region.address == fake_physical+0x100000 && region.size == PAGE_SIZE);
        assert(ext[0]==4 && ext[1]==fake_physical+128 && ext[2]==24 && ext[3]==0);
        if (assigns++ == 0) {
            assert(!protected_target && src==3 && dest.vmid==10 && dest.permission==6);
            protected_target=true;
            if (scenario==10) r.status=-5;
            if (scenario==12) r.status=2;
        } else {
            assert(protected_target && src==10 && dest.vmid==3 && dest.permission==7);
            if (scenario==11) r.value[0]=3;
            else protected_target=false;
        }
    } else assert(!"unexpected SCM function");
    return r;
}
'''
production = production[:start] + provider + production[end:]
stub = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uintptr_t vm_offset_t;
#define PAGE_MASK (PAGE_SIZE - 1)
#define KMA_KOBJECT 1
#define KMA_ZERO 2
#define KMA_DATA 4
#define VM_KERN_MEMORY_IOKIT 1
#define KERN_SUCCESS 0
static bool adreno8_reset_required;
static int scenario, queries, enables, creates, deletes, publishes, allocations, assigns;
static bool protected_target;
static int kernel_map;
static void *allocated[2];
static uint64_t fake_physical = 0x104000;
static bool PE_parse_boot_argn(const char *name, void *data, size_t size) {
    assert(!strcmp(name,"adreno8_secure_probe") && size==4);
    *(uint32_t*)data=scenario>=9 ? 2 : 1; return true;
}
static int adreno8_say(const char *format, ...) { (void)format; return 0; }
static int kmem_alloc(int map, vm_offset_t *va, size_t size, int flags, int tag) {
    (void)map; (void)flags; (void)tag;
    allocations++;
    if (scenario == 3) return -1;
    assert(size == PAGE_SIZE);
    assert(allocations<=2);
    assert(posix_memalign(&allocated[allocations-1], PAGE_SIZE, PAGE_SIZE) == 0);
    memset(allocated[allocations-1], 0, PAGE_SIZE); *va = (uintptr_t)allocated[allocations-1]; return 0;
}
static uint64_t kvtophys(vm_offset_t va) {
    for(unsigned i=0;i<2;i++) if(va==(uintptr_t)allocated[i]) {
        if(i==1) assert(!protected_target);
        return fake_physical+0x100000*i;
    }
    assert(!"unknown allocation"); return 0;
}
static void adreno8_to_memory(vm_offset_t va, size_t size) {
    assert(size==PAGE_SIZE && (va==(uintptr_t)allocated[0] || va==(uintptr_t)allocated[1]));
    if(va==(uintptr_t)allocated[1]) assert(!protected_target);
    publishes++;
}
'''
main = r'''
int main(int argc, char **argv) {
    assert(argc == 2); scenario = atoi(argv[1]);
    if (scenario == 7) fake_physical++;
    if (scenario == 8) fake_physical = 1ULL << 36;
    bool ok = adreno8_secure_diagnostic();
    assert(ok == (scenario == 0 || scenario == 9));
    assert(adreno8_reset_required && queries == 5);
    assert(enables == (scenario != 1));
    assert(allocations == (scenario>=9 && scenario<=12 ? 2 : (scenario != 1 && scenario != 2)));
    assert(creates == (scenario == 0 || scenario == 4 || scenario == 5 || scenario == 6 || scenario>=9));
    assert(deletes == (scenario == 0 || scenario == 5 || scenario == 9));
    assert(assigns == (scenario==9 || scenario==11 ? 2 : (scenario==10 || scenario==12)));
    assert(!adreno8_secure_diagnostic()); /* No replay of an unknown transaction. */
    for(unsigned i=0;i<2;i++) free(allocated[i]); /* Final cold reset in the test model. */
}
'''
with tempfile.TemporaryDirectory(prefix='op13-secure-bridge-') as directory:
    source = Path(directory) / 'check.c'
    source.write_text(stub + production + main)
    for page in [4096, 16384]:
        binary = Path(directory) / f'check-{page}'
        subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200112L', f'-DPAGE_SIZE={page}',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I', str(driver.parent), str(source), '-o', str(binary)], check=True)
        for scenario in range(14): subprocess.run([str(binary), str(scenario)], check=True)
        print(f'PAGE_SIZE={page}: bridge ABI, full-width handle, failures and retained ownership: PASS')
