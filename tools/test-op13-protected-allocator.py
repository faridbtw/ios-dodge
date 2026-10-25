#!/usr/bin/env python3
"""Run the production protected-record allocator through success and retained failures."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
DEV = ROOT/'source/xnu-build-snapshot/xnu-8796.121.2/bsd/dev'
driver = (DEV/'adreno8_protected.c').read_text()
driver = '\n'.join(l for l in driver.splitlines() if not l.startswith('#include'))
wire = (DEV/'adreno8_hwsched_private.h').read_text()
a=wire.index('struct adreno8_hfi_mem_request');b=wire.index('/* The allocation owner',a)
stub = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
#define PAGE_SIZE (1u<<PAGE_SHIFT)
#define KMA_KOBJECT 1
#define KMA_ZERO 2
#define KMA_DATA 4
#define VM_KERN_MEMORY_IOKIT 0
#define KERN_SUCCESS 0
#define ADRENO8_MEM_READ 1
#define ADRENO8_MEM_WRITE 2
#define ADRENO8_MEM_PRIV 8
static int kernel_map, scenario;
static bool adreno8_gpu8_keep=true, adreno8_gpu8_return, adreno8_reset_required;
static unsigned allocations, maps, protects, enables;
static bool protected_data;
struct adreno8_gpu_space { const char *name; unsigned context; bool secure; };
static int adreno8_say(const char *s, ...) { return 0; }
static bool PE_parse_boot_argn(const char *name,void *p,size_t n) {
    assert(!strcmp(name,"adreno8_protected") && n==4); *(uint32_t*)p=scenario==1 ? 0 : 1; return true;
}
static int kmem_alloc(int map,vm_offset_t *memory,size_t size,int flags,int tag) {
    allocations++; if(scenario==4) return -1;
    *memory=(uintptr_t)calloc(1,size); assert(*memory); return 0;
}
static uintptr_t adreno8_space_new_table(struct adreno8_gpu_space *s,unsigned level,unsigned index) {
    assert(s->secure && s->context==3 && !level && !index); return scenario==6 ? 0 : 1;
}
static bool adreno8_space_map_region(struct adreno8_gpu_space *s,uint64_t address,uintptr_t memory,uint32_t size,uint32_t flags) {
    assert(!protected_data && s->secure && address==0x100000000ULL && memory && size==0x138000 && flags==11);
    maps++; return scenario!=5;
}
static bool adreno8_secure_protect(uintptr_t memory,uint32_t size,bool tables) {
    assert(!protected_data && memory && size==0x138000 && !tables); protects++;
    protected_data=true; return scenario!=2;
}
static bool adreno8_space_on(struct adreno8_gpu_space *s,unsigned stream,unsigned match) {
    assert(protected_data && s->secure && s->context==3 && stream==2 && match==2);
    enables++; return scenario!=3;
}
static bool adreno8_space_faulted(struct adreno8_gpu_space *s) { return false; }
'''
tests=r'''
#undef printf
int main(int argc,char **argv) {
    assert(argc==2); scenario=atoi(argv[1]);
    struct adreno8_hfi_mem_request q={.kind=5,.handle=5,.flags=0x1007,.size=0x138000};
    struct adreno8_hfi_mem_result result={0};
    q.flags=7; assert(!adreno8_protected_alloc(&q,&result) && !allocations); q.flags=0x1007;
    q.size=0xffffffff; assert(!adreno8_protected_alloc(&q,&result) && !allocations); q.size=0x138000;
    q.align=0x10000; assert(!adreno8_protected_alloc(&q,&result) && !allocations); q.align=0;
    bool ok=adreno8_protected_alloc(&q,&result);
    assert(ok==(scenario==0));
    if(ok) {
        assert(result.secure && result.gpu_address==0x100000000ULL && !result.gmu_address && result.size==q.size);
        struct adreno8_hfi_mem_result replay={0};
        assert(adreno8_protected_alloc(&q,&replay) && !memcmp(&result,&replay,sizeof result));
        q.size+=4096; assert(!adreno8_protected_alloc(&q,&replay)); q.size-=4096;
        q.handle++; assert(!adreno8_protected_alloc(&q,&replay)); q.handle--;
        assert(allocations==1 && maps==1 && protects==1 && enables==1);
    } else {
        unsigned count=allocations;
        assert(!adreno8_protected_alloc(&q,&result) && allocations==count);
        assert(!result.secure && !complete);
        if(scenario==1) assert(!allocations && !adreno8_reset_required);
        else assert(adreno8_reset_required && allocations==1);
    }
    /* Only the test harness releases allocations after the whole lifecycle. */
    free((void*)secure_memory);
    printf("protected allocator scenario %d: PASS\n",scenario);
}
'''
with tempfile.TemporaryDirectory(prefix='op13-protected-') as directory:
    p=Path(directory)/'check.c';p.write_text(stub+wire[a:b]+driver+tests)
    for shift in [12,14]:
        binary=Path(directory)/f'check-{shift}'
        subprocess.run(['cc','-std=c11',f'-DPAGE_SHIFT={shift}','-fsanitize=address,undefined',
            '-fno-omit-frame-pointer',str(p),'-o',str(binary)],check=True)
        for scenario in range(7): subprocess.run([str(binary),str(scenario)],check=True)
        print(f'PAGE_SIZE={1<<shift}: protected record, replay and retained failures: PASS')
