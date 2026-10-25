/* MP_ASSIGN SMC64 metadata, pinned to OnePlusOSS kernel 6028f47f.
 * Layout is little endian on the target. This helper changes no ownership.
 */
#ifndef ADRENO8_SECURE_WIRE_H
#define ADRENO8_SECURE_WIRE_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

struct a8_secure_region { uint64_t address, size; };
struct a8_secure_permission {
    uint32_t vmid, permission;
    uint64_t context;
    uint32_t context_size, reserved;
};
_Static_assert(sizeof(struct a8_secure_region) == 16, "MP_ASSIGN region ABI");
_Static_assert(sizeof(struct a8_secure_permission) == 24, "MP_ASSIGN permission ABI");
_Static_assert(offsetof(struct a8_secure_permission, context) == 8, "MP_ASSIGN context ABI");

static inline bool
a8_secure_assign_one(void *metadata, size_t capacity, uint64_t metadata_pa,
    uint64_t target_pa, uint64_t size, uint32_t source, uint32_t destination,
    uint32_t permission, uint64_t registers[4])
{
    /* Only the two ownership transitions used by the disposable-page test. */
    if (!metadata || !registers || capacity < 4096 || capacity > 65536 ||
        (capacity & (capacity - 1)) || (metadata_pa & 4095) || (target_pa & 4095) ||
        !size || (size & 4095) || metadata_pa >= (1ULL << 36) ||
        capacity > (1ULL << 36) - metadata_pa || target_pa >= (1ULL << 36) ||
        size > (1ULL << 36) - target_pa ||
        (target_pa < metadata_pa + capacity && target_pa + size > metadata_pa) ||
        !((source == 3 && destination == 10 && permission == 6) ||
          (source == 10 && destination == 3 && permission == 7))) return false;
    memset(metadata, 0, capacity);
    uint8_t *bytes = metadata;
    struct a8_secure_region region = { target_pa, size };
    struct a8_secure_permission dest = { .vmid = destination, .permission = permission };
    uint64_t extra[7] = { sizeof(source), metadata_pa + 128, sizeof(dest), 0, 0, 0, 0 };
    memcpy(bytes, &source, sizeof(source));
    memcpy(bytes + 64, &region, sizeof(region));
    memcpy(bytes + 128, &dest, sizeof(dest));
    memcpy(bytes + 192, extra, sizeof(extra));
    registers[0] = metadata_pa + 64;
    registers[1] = sizeof(region);
    registers[2] = metadata_pa;
    registers[3] = metadata_pa + 192; /* x5 points to arguments 3..9, not to argument 3. */
    return true;
}
/* Forward assignment of exclusive data or shared page tables.  The metadata
 * page remains HLOS-owned and registered with QTEE for the transaction. */
static inline bool
a8_secure_assign_regions(void *metadata, size_t capacity, uint64_t metadata_pa,
    const struct a8_secure_region *regions, size_t count, bool page_tables,
    uint64_t registers[4])
{
    if (!metadata || !regions || !registers || capacity < 4096 || capacity > 65536 ||
        (capacity & (capacity - 1)) || (metadata_pa & 4095) ||
        metadata_pa >= (1ULL << 36) || capacity > (1ULL << 36) - metadata_pa ||
        !count || count > 128) return false;
    for (size_t i = 0; i < count; i++) {
        uint64_t pa = regions[i].address, size = regions[i].size;
        if (!pa || (pa & 4095) || !size || (size & 4095) || pa >= (1ULL << 36) ||
            size > (1ULL << 36) - pa ||
            (pa < metadata_pa + capacity && pa + size > metadata_pa)) return false;
        for (size_t j = 0; j < i; j++)
            if (pa < regions[j].address + regions[j].size &&
                pa + size > regions[j].address) return false;
    }
    uint32_t source = 3;
    struct a8_secure_permission dest[2] = {
        { .vmid = page_tables ? 3 : 10, .permission = 6 },
        { .vmid = 10, .permission = 4 }
    };
    size_t destinations = page_tables ? 2 : 1;
    uint64_t extra[7] = { 4, metadata_pa + 2112,
        destinations * sizeof(dest[0]), 0, 0, 0, 0 };
    memset(metadata, 0, capacity);
    memcpy(metadata, &source, sizeof(source));
    memcpy((uint8_t *)metadata + 64, regions, count * sizeof(regions[0]));
    memcpy((uint8_t *)metadata + 2112, dest, destinations * sizeof(dest[0]));
    memcpy((uint8_t *)metadata + 2176, extra, sizeof(extra));
    registers[0] = metadata_pa + 64;
    registers[1] = count * sizeof(regions[0]);
    registers[2] = metadata_pa;
    registers[3] = metadata_pa + 2176;
    return true;
}
#endif
