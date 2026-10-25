/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef SM8750_AVDD_H
#define SM8750_AVDD_H
#include "efi.h"
struct sm8750_avdd_session {
 uint8_t original[7],configured[7],subtype,owned;
 uint32_t result,stage;
};
/* Pinned SPMI1.3, bus0/SID9/GPIO3 ONLY. Requires existing VIN1, pulldown,
 * push-pull and low strength, as captured on this phone. It never changes
 * a supply selector or drive strength. Only LV/MV type is supported.
 * Explicit enable needed; session must be retained for UEFI-context restore.
 */
int sm8750_loader_avdd_enable(efi_boot_services *,int,struct sm8750_avdd_session *);
int sm8750_loader_avdd_restore(efi_boot_services *,struct sm8750_avdd_session *);
#endif
