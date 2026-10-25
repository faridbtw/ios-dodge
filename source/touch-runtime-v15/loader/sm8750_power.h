/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef SM8750_POWER_H
#define SM8750_POWER_H
#include "efi.h"
struct sm8750_power_session {
 void *npa_clients[3];
 uint64_t npa_revision, status;
 uint32_t stage;
};
/* Loader-only experimental NPA votes, never call from XNU.
 * enabled=0: no calls. This establishes requests, NOT measured rail readiness.
 * No AVDD or GENI/ICC ownership is claimed. On success retain the session
 * until native ownership is relinquished, then call release in UEFI context.
 */
int sm8750_loader_power_vote(efi_boot_services *,int enabled,struct sm8750_power_session *);
void sm8750_loader_power_release(efi_boot_services *,struct sm8750_power_session *);
#endif
