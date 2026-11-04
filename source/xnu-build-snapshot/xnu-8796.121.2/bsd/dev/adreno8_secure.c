/* Dodge secure-memory bring-up. Vendor ABI: OnePlusOSS kernel 6028f47f.
 * Diagnostics verify registration and disposable-page ownership roundtrips.
 * The opt-in protected allocator assigns real GPU data and shared page tables.
 * All allocations remain owned by this kernel until its explicit cold reset.
 */
#include "adreno8_private.h"
#include "adreno8_secure_wire.h"

#define printf adreno8_say
#define SCM_AVAILABLE 0x42000601ULL
#define SCM_BRIDGE_ENABLE 0x42000c1cULL
#define SCM_BRIDGE_DELETE 0x42000c1dULL
#define SCM_BRIDGE_CREATE 0x42000c1eULL
#define SCM_ASSIGN 0x42000c16ULL

struct a8_scm_result { int32_t status; uint64_t value[3]; };

static struct a8_scm_result
secure_scm(uint64_t function, uint64_t info, uint64_t a0, uint64_t a1,
    uint64_t a2, uint64_t a3)
{
    uint64_t session = 0;
    struct a8_scm_result result = { .status = -1 };
    for (unsigned attempt = 0; attempt < 100; attempt++) {
        register uint64_t x0 __asm__("x0") = attempt == 0 ? function : 1;
        register uint64_t x1 __asm__("x1") = info;
        register uint64_t x2 __asm__("x2") = a0;
        register uint64_t x3 __asm__("x3") = a1;
        register uint64_t x4 __asm__("x4") = a2;
        register uint64_t x5 __asm__("x5") = a3;
        register uint64_t x6 __asm__("x6") = session;
        register uint64_t x7 __asm__("x7") = 0;
        __asm__ volatile ("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3),
            "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7) : : "x8", "x9", "x10",
            "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
        session = x6;
        if (x0 == 1) continue;
        result.status = (int32_t)x0;
        result.value[0] = x1; result.value[1] = x2; result.value[2] = x3;
        printf("adreno8: secure SCM %#llx: status %d values %#llx %#llx %#llx\n",
            function, result.status, x1, x2, x3);
        if (x0 == 2 || x0 == 3)
            printf("adreno8: secure SCM WAITQ response; memory retained, no successful assignment claimed\n");
        return result;
    }
    printf("adreno8: secure SCM continuation limit; memory retained\n");
    return result;
}

static bool
secure_ownership_roundtrip(vm_offset_t metadata, uint64_t metadata_pa)
{
    static vm_offset_t target;
    if (kmem_alloc(kernel_map, &target, PAGE_SIZE,
        KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) return false;
    uint64_t physical = kvtophys(target), args[4];
    if (!a8_secure_assign_one((void *)metadata, PAGE_SIZE, metadata_pa,
        physical, PAGE_SIZE, 3, 10, 6, args)) return false;
    adreno8_to_memory(target, PAGE_SIZE);
    adreno8_to_memory(metadata, PAGE_SIZE);
    struct a8_scm_result r = secure_scm(SCM_ASSIGN, 0x1117, args[0], args[1], args[2], args[3]);
    if (r.status || r.value[0]) {
        printf("adreno8: disposable-page assignment did not complete; ownership unknown, retain until cold reset\n");
        return false;
    }
    printf("adreno8: disposable page assigned HLOS -> CP_PIXEL; host does not access its contents\n");
    /* Only metadata remains host-accessible. Never inspect the protected target. */
    if (!a8_secure_assign_one((void *)metadata, PAGE_SIZE, metadata_pa,
        physical, PAGE_SIZE, 10, 3, 7, args)) return false;
    adreno8_to_memory(metadata, PAGE_SIZE);
    r = secure_scm(SCM_ASSIGN, 0x1117, args[0], args[1], args[2], args[3]);
    if (r.status || r.value[0]) {
        printf("adreno8: disposable-page return to HLOS did not complete; retain until cold reset\n");
        return false;
    }
    printf("adreno8: disposable-page ownership roundtrip CP_PIXEL -> HLOS completed; allocation retained until reset\n");
    return true;
}

bool
adreno8_secure_diagnostic(void)
{
    /* Called only inside the stage-14 CPU6/live-log/cold-reset gate. */
    static vm_offset_t metadata;
    static bool attempted;
    if (attempted) return false;
    attempted = true;
    adreno8_reset_required = true;
    uint32_t mode = 1;
    PE_parse_boot_argn("adreno8_secure_probe", &mode, sizeof(mode));
    if (mode != 1 && mode != 2) return false;
    const uint32_t functions[] = { 0x02000c16, 0x02000c1c, 0x02000c1d,
        0x02000c1e, 0x02002403 };
    bool bridge_functions = true, assignment_available = false;
    for (unsigned i = 0; i < sizeof(functions) / sizeof(functions[0]); i++) {
        struct a8_scm_result r = secure_scm(SCM_AVAILABLE, 1, functions[i], 0, 0, 0);
        printf("adreno8: secure service %#x available=%llu (status %d)\n",
            functions[i], r.value[0], r.status);
        if (i == 0) assignment_available = r.status == 0 && r.value[0] == 1;
        if (i >= 1 && i <= 3 && (r.status || r.value[0] != 1)) bridge_functions = false;
    }
    if (!bridge_functions) {
        printf("adreno8: QTEE bridge services unavailable; no registration attempted\n");
        return false;
    }
    struct a8_scm_result r = secure_scm(SCM_BRIDGE_ENABLE, 0, 0, 0, 0, 0);
    if (r.status || r.value[0]) return false;
    if (kmem_alloc(kernel_map, &metadata, PAGE_SIZE,
        KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) return false;
    uint64_t physical = kvtophys(metadata);
    if ((physical & (PAGE_SIZE - 1)) || physical >= (1ULL << 36)) return false;
    adreno8_to_memory(metadata, PAGE_SIZE);
    /* qtee_shmbridge.c: HLOS VMID=3, NS RW=6, TZ RW=6, destination count << 9.
     * Captured Dodge DT has no qcom,support-hypervisor property.
     */
    r = secure_scm(SCM_BRIDGE_CREATE, 4, physical | 6, physical | 6,
        (uint64_t)PAGE_SIZE | (1ULL << 9), 3);
    if (r.status || r.value[0]) return false;
    uint64_t handle = r.value[1];
    printf("adreno8: QTEE metadata bridge registered, PA %#llx size %#lx handle %#llx\n",
        physical, (unsigned long)PAGE_SIZE, handle);
    if (mode == 2 && (!assignment_available || !secure_ownership_roundtrip(metadata, physical))) return false;
    r = secure_scm(SCM_BRIDGE_DELETE, 1, handle, 0, 0, 0);
    if (r.status) return false;
    printf("adreno8: QTEE metadata bridge deregistered; GPU buffer ownership unchanged\n");
    return true;
}

/* Actual protected allocations, retained along with their QTEE bridge until reset.
 * The stage-14 caller is serialized and explicitly opts into this path. */
bool
adreno8_secure_protect(vm_offset_t memory, uint32_t size, bool page_tables)
{
    static vm_offset_t metadata;
    static bool ready, failed;
    struct a8_secure_region regions[128];
    uint64_t args[4];
    uint32_t opt = 0;
    if (!PE_parse_boot_argn("adreno8_protected", &opt, sizeof(opt)) || opt != 1 ||
        failed || !memory || (memory & PAGE_MASK) || !size || (size & PAGE_MASK) ||
        size / PAGE_SIZE > 128) return false;
    adreno8_reset_required = true;
    if (!ready) {
        failed = true; /* Remains latched for every uncertain/partial transaction. */
        const uint32_t services[] = {0x02000c16,0x02000c1c,0x02000c1d,0x02000c1e};
        for (unsigned i=0; i<4; i++) {
            struct a8_scm_result r = secure_scm(SCM_AVAILABLE,1,services[i],0,0,0);
            if (r.status || r.value[0] != 1) return false;
        }
        struct a8_scm_result r = secure_scm(SCM_BRIDGE_ENABLE,0,0,0,0,0);
        if (r.status || r.value[0]) return false;
        if (kmem_alloc(kernel_map, &metadata, PAGE_SIZE,
            KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) return false;
        uint64_t physical = kvtophys(metadata);
        if (!physical || (physical & PAGE_MASK) || physical >= (1ULL<<36)) return false;
        adreno8_to_memory(metadata,PAGE_SIZE);
        r = secure_scm(SCM_BRIDGE_CREATE,4,physical|6,physical|6,
            (uint64_t)PAGE_SIZE|(1ULL<<9),3);
        if (r.status || r.value[0]) return false;
        printf("adreno8: protected allocator QTEE bridge %#llx retained until cold reset\n",r.value[1]);
        ready = true; failed = false;
    }
    for (unsigned i=0; i<size/PAGE_SIZE; i++) {
        regions[i].address = kvtophys(memory+(vm_offset_t)i*PAGE_SIZE);
        regions[i].size = PAGE_SIZE;
    }
    if (!a8_secure_assign_regions((void *)metadata,PAGE_SIZE,kvtophys(metadata),
        regions,size/PAGE_SIZE,page_tables,args)) return false;
    adreno8_to_memory(memory,size);
    /* Match secure_buffer.c's BATCH_MAX_SECTIONS=32. A partial failure never
     * accesses or frees any target page, and reset is the only cleanup. */
    unsigned regions_count = size/PAGE_SIZE;
    for (unsigned start=0; start<regions_count; start+=32) {
        unsigned count = regions_count-start;
        if (count>32) count=32;
        if (!a8_secure_assign_regions((void *)metadata,PAGE_SIZE,kvtophys(metadata),
            regions+start,count,page_tables,args)) { failed=true; return false; }
        adreno8_to_memory(metadata,PAGE_SIZE);
        struct a8_scm_result r = secure_scm(SCM_ASSIGN,0x1117,args[0],args[1],args[2],args[3]);
        if (r.status || r.value[0]) {
            failed = true;
            printf("adreno8: protected assignment incomplete at region %u; retain until cold reset\n",start);
            return false;
        }
    }
    printf("adreno8: %s ownership assigned: %u bytes, %u physical regions\n",
        page_tables ? "HLOS RW + CP_PIXEL R page-table" : "CP_PIXEL RW data",size,size/PAGE_SIZE);
    return true;
}
