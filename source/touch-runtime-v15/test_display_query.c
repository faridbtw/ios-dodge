#include <assert.h>
#include <stdlib.h>
#include "sm8750_display_query.c"

static unsigned queries, frees, reads;
static efi_status never_set_mode(struct efi_gop *gop, uint32_t mode)
{
    (void)gop; (void)mode;
    assert(!"Display discovery must never change mode");
    return 1;
}
static efi_status query(struct efi_gop *gop, uint32_t mode, uint64_t *size,
    efi_gop_mode_info **info)
{
    (void)gop;
    queries++;
    *info = calloc(1, sizeof(**info));
    assert(*info);
    (*info)->width = 1440; (*info)->height = 3168;
    (*info)->pixels_per_scanline = 1440;
    *size = mode == 3 ? 1 : sizeof(**info);
    return 0;
}
static efi_status release(void *pointer) { frees++; free(pointer); return 0; }
static efi_status get(uint32_t property, void *data, uint32_t *size)
{
    assert(property == 0 && *size == sizeof(uint64_t));
    reads++;
    *(uint64_t *)data = 0xa0000000;
    return 0;
}
static struct display_utils utils = {.revision=0x20000, .get_property=get};
static efi_status locate(const efi_guid *guid, void *registration, void **out)
{
    (void)registration;
    assert(guid->a == 0xc0dd69ac);
    *out = &utils;
    return 0;
}
int main(void)
{
    efi_boot_services bs = {.locate_protocol=locate, .free_pool=release};
    efi_gop_mode mode = {.max_mode=100, .mode=2};
    efi_gop gop = {.query_mode=query, .set_mode=never_set_mode, .mode=&mode};
    struct sm8750_display_report report;
    assert(sm8750_display_query(NULL, &gop, &report) == -1);
    assert(!queries && !reads);
    assert(!sm8750_display_query(&bs, &gop, &report));
    assert(queries == 16 && frees == 16 && reads == 1);
    assert(report.available_modes == 100 && report.queried_modes == 16);
    assert(report.modes[0].width == 1440 && report.modes[0].height == 3168);
    assert(EFI_ERROR(report.modes[3].status) && report.modes[3].width == 0);
    assert(report.device_tree_address == 0xa0000000);
    utils.revision = 0x30000;
    assert(!sm8750_display_query(&bs, NULL, &report));
    assert(reads == 1 && EFI_ERROR(report.tree_status));
    bs.free_pool = NULL;
    assert(!sm8750_display_query(&bs, &gop, &report));
    assert(queries == 16);
    return 0;
}
