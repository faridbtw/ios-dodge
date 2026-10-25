/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef SM8750_PREPARE_H
#define SM8750_PREPARE_H
#include "efi.h"

/* Diagnostics only, deliberately NOT a resource-ready certificate. */
struct sm8750_prepare_report {
 uint64_t clock_revision, spmi_revision;
 uint64_t clock_locate_status, spmi_locate_status;
 uint64_t clock_status[3];
 uint64_t clock_id_status[3],clock_frequency_status[3],clock_on_status[3];
 uint64_t clock_id[3];
 uint32_t frequency[3], spmi_result, spmi_bytes;
 uint8_t clock_on[3], clock_votes, reserved[4];
 uint8_t avdd_type_subtype[2]; /* SID9 GPIO3 0x8a04/05 */
 uint8_t avdd_control[8];      /* 0x8a40..47, no writes */
};
/* enabled=0 makes zero firmware calls. clocks=0 only locates protocols,
 * queries clock IDs/rates/states and reads GPIO3 identity/control. clocks=1
 * additionally obtains clock enable votes; source rates are NOT changed.
 * report never asserts power/ICC/exclusive ownership or touchscreen readiness.
 * All firmware calls must run in the loader, on its UEFI stack and context.
 */
int sm8750_loader_prepare(efi_boot_services *,int enabled,int clocks,
                         struct sm8750_prepare_report *);
void sm8750_loader_release_clocks(efi_boot_services *,struct sm8750_prepare_report *);
#endif
