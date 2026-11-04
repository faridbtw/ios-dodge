#ifndef SM8750_DISPLAY_QUERY_H
#define SM8750_DISPLAY_QUERY_H
#include "efi.h"

#define SM8750_DISPLAY_QUERY_MODES 16
struct sm8750_display_mode {
    efi_status status;
    uint32_t width, height, pixel_format, stride_pixels;
};
struct sm8750_display_report {
    efi_status utils_status, tree_status;
    uint64_t utils_revision, device_tree_address;
    uint32_t current_mode, available_modes, queried_modes;
    struct sm8750_display_mode modes[SM8750_DISPLAY_QUERY_MODES];
};

/* Queries only. GOP contains no refresh-rate field; no Hz value is inferred. */
int sm8750_display_query(efi_boot_services *, efi_gop *, struct sm8750_display_report *);
#endif
