/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GENI_PREFLIGHT_H
#define GENI_PREFLIGHT_H
#include "efi.h"
#include "fifo_snapshot.h"
struct geni_preflight {
 uint64_t ids[5],id_status[5],enable_status[5],on_status[5],disable_status[5];
 uint64_t enabled_status[5];
 uint8_t votes,on[5],initial_enabled[5];
 uint32_t if_disable,fw_revision,reads;
 int result,cleanup_result;
 struct fifo_snapshot snapshot;
};
/* Zero-init once. Callback must validate device-memory mapping and accept
 * ONLY these exact addresses; this module exposes no arbitrary-register API. */
int geni_preflight_run(efi_boot_services *,int enabled,
 int (*read32)(void *,uint64_t,uint32_t *),void *,struct geni_preflight *);
int geni_preflight_release(efi_boot_services *,struct geni_preflight *);
#endif
