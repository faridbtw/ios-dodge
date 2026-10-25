/* Dodge HWSCHED protected context record. One allocation, held until cold reset.
 * SID 2 is gfx3d_secure in the captured DT. Vendor reserves contexts 0/1 for
 * GPU user/LPAC; GMU already owns 2, so use verified-idle context 3/match 2.
 */
#include "adreno8_memory_private.h"
#include "adreno8_hwsched_private.h"
#define printf adreno8_say
static struct adreno8_gpu_space secure_space = { .name = "secure GPU", .context = 3, .secure = true };
static struct adreno8_hfi_mem_request secure_request;
static struct adreno8_hfi_mem_result secure_result;
static vm_offset_t secure_memory;
static bool attempted, complete;

bool
adreno8_protected_alloc(const struct adreno8_hfi_mem_request *request,
    struct adreno8_hfi_mem_result *result)
{
    uint32_t opt = 0;
    if (!request || !result || !PE_parse_boot_argn("adreno8_protected", &opt, sizeof(opt)) ||
        opt != 1 || !adreno8_gpu8_keep || adreno8_gpu8_return ||
        request->kind != 5 || request->flags != 0x1007 || !request->size ||
        request->size > 2*1024*1024 || (request->align & ~0xffffu)) return false;
    if (attempted) {
        if (!complete || request->kind != secure_request.kind || request->handle != secure_request.handle ||
            request->size != secure_request.size || request->flags != secure_request.flags ||
            request->align != secure_request.align || adreno8_space_faulted(&secure_space)) return false;
        *result = secure_result;
        return true;
    }
    unsigned va_shift = request->align & 255, size_shift = request->align >> 8;
    if (va_shift > 30 || size_shift > 21) return false;
    if (size_shift < PAGE_SHIFT) size_shift = PAGE_SHIFT;
    uint64_t size = ((uint64_t)request->size + (1ULL<<size_shift)-1) & ~((1ULL<<size_shift)-1);
    if (!size || size > 2*1024*1024) return false;
    attempted = true;
    adreno8_reset_required = true;
    secure_request = *request;
    /* The vendor's 64-bit secure VA range begins at 4 GiB. */
    secure_result.gpu_address = 0x100000000ULL;
    secure_result.size = (uint32_t)size;
    if (kmem_alloc(kernel_map,&secure_memory,(vm_size_t)size,
        KMA_KOBJECT|KMA_ZERO|KMA_DATA,VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) return false;
    if (!adreno8_space_new_table(&secure_space,0,0) ||
        !adreno8_space_map_region(&secure_space,secure_result.gpu_address,secure_memory,
            (uint32_t)size,ADRENO8_MEM_READ|ADRENO8_MEM_WRITE|ADRENO8_MEM_PRIV) ||
        !adreno8_secure_protect(secure_memory,(uint32_t)size,false) ||
        !adreno8_space_on(&secure_space,2,2)) return false;
    /* No host access to secure_memory occurs after the ownership transition. */
    secure_result.secure = true;
    complete = true;
    *result = secure_result;
    printf("adreno8: protected context record ready: GPU %#llx size %#x, SID 2/CB 3\n",
        result->gpu_address,result->size);
    return true;
}

bool
adreno8_protected_faulted(void)
{
    return attempted && (!complete || adreno8_space_faulted(&secure_space));
}
