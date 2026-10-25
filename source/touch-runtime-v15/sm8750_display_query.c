/* Read-only display discovery. Protocol layout: Mu-Silicium EFIDisplayUtils.h.
 * Deliberately never calls SetMode/SetProperty/SetVariable or accesses MMIO.
 */
#include "sm8750_display_query.h"

#define NOT_FOUND (EFI_ERROR_BIT | 14)
#define BAD_DATA (EFI_ERROR_BIT | 2)
#define UNSUPPORTED (EFI_ERROR_BIT | 3)
struct display_utils {
    uint64_t revision;
    void *set_property;
    efi_status (EFIAPI *get_property)(uint32_t, void *, uint32_t *);
    void *render_logo, *set_mode, *set_variable, *get_variable;
};
_Static_assert(offsetof(struct display_utils, get_property) == 16, "DisplayUtils ABI");

int sm8750_display_query(efi_boot_services *bs, efi_gop *gop,
    struct sm8750_display_report *report)
{
    static const efi_guid utils_guid = {0xc0dd69ac, 0x76ba, 0x11e6,
        {0xab, 0x24, 0x1f, 0xc7, 0xf5, 0x57, 0x5f, 0x19}};
    struct display_utils *utils = NULL;
    if (!report) return -1;
    *report = (struct sm8750_display_report){0};
    report->utils_status = report->tree_status = NOT_FOUND;
    if (!bs || !bs->locate_protocol) return -1;
    if (gop && gop->mode) {
        report->current_mode = gop->mode->mode;
        report->available_modes = gop->mode->max_mode;
        if (gop->query_mode && bs->free_pool) {
            uint32_t count = report->available_modes;
            if (count > SM8750_DISPLAY_QUERY_MODES) count = SM8750_DISPLAY_QUERY_MODES;
            for (uint32_t i = 0; i < count; i++) {
                uint64_t size = 0;
                efi_gop_mode_info *info = NULL;
                struct sm8750_display_mode *mode = &report->modes[i];
                mode->status = gop->query_mode(gop, i, &size, &info);
                if (!EFI_ERROR(mode->status)) {
                    if (!info || size < sizeof(*info)) mode->status = BAD_DATA;
                    else {
                        mode->width = info->width;
                        mode->height = info->height;
                        mode->pixel_format = info->pixel_format;
                        mode->stride_pixels = info->pixels_per_scanline;
                    }
                }
                if (info) bs->free_pool(info);
                report->queried_modes++;
            }
        }
    }
    report->utils_status = bs->locate_protocol(&utils_guid, NULL, (void **)&utils);
    if (!EFI_ERROR(report->utils_status) && utils) {
        report->utils_revision = utils->revision;
        /* Only the documented v2 ABI is used. Returned FDT address is not dereferenced. */
        if (utils->revision >= 0x20000 && utils->revision < 0x30000 && utils->get_property) {
            uint32_t size = sizeof(report->device_tree_address);
            report->tree_status = utils->get_property(0, &report->device_tree_address, &size);
            if (!EFI_ERROR(report->tree_status) && size != sizeof(report->device_tree_address)) {
                report->device_tree_address = 0;
                report->tree_status = BAD_DATA;
            }
        } else report->tree_status = UNSUPPORTED;
    }
    return 0;
}
