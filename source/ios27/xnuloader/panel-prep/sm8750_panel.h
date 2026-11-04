/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef SM8750_PANEL_H
#define SM8750_PANEL_H
#include "../loader/sm8750_power.h"
#include "../loader/sm8750_avdd.h"
/* Only offsets a1000/a1004/a2000/a2004 of GPIO bank f100000 are permitted.
 * Backend must supply ordered device-memory accesses; return nonzero on failure. */
struct sm8750_panel_io {
 void *cookie;
 int (*read)(void *, uint32_t, uint32_t *);
 int (*write)(void *, uint32_t, uint32_t);
};
struct sm8750_panel {
 efi_boot_services *bs;
 struct sm8750_panel_io io;
 struct sm8750_power_session power;
 struct sm8750_avdd_session avdd;
 uint32_t saved_cfg[2], saved_io[2], observed_cfg[2], observed_io[2];
 unsigned pins_owned, prepared, stage, power_settle_pending;
 int error, cleanup_error;
};
/* Zero-init once; retain the entire session after any failure until release
 * succeeds. Success means NPA votes + GPIO readbacks, NOT measured rail voltage
 * or controller readiness. Does not configure SPI or interrupt routing. */
int sm8750_panel_prepare(struct sm8750_panel *, efi_boot_services *,
 const struct sm8750_panel_io *, int enabled);
int sm8750_panel_release(struct sm8750_panel *);
/* Optional ARM64 identity-mapped UEFI backend, never implicitly enabled. */
int sm8750_panel_raw_read(void *, uint32_t, uint32_t *);
int sm8750_panel_raw_write(void *, uint32_t, uint32_t);
#endif
