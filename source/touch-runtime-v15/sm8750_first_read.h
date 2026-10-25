/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef SM8750_FIRST_READ_H
#define SM8750_FIRST_READ_H
#include "efi.h"
struct sm8750_first_read_session {
 void *handle;
 unsigned panel_owned,blocked;
 unsigned limit; /* 1=read-only,2=panel,3=open-close,4=header,5=continuation */
 uint32_t (*close_owned)(void *);
};
struct sm8750_first_read_report {
 uint32_t open_status,read_status[2],close_status,header_valid;
 int32_t topology_status,image_status,panel_status,release_status;
 uint32_t instance,se_flags;
 int32_t flags_status;
 /* Transfer completion is separate from protocol validation. Extension-bearing
  * startup packets are retained raw, never certified without CRC semantics. */
 uint32_t raw_transfer_complete,frame_valid,identify_valid;
 int32_t packet_status;
 uint8_t header[4],continuation[31];
};
/* prepare must establish supplies/reset/IRQ; release restores its own state.
 * image is an actual LoadedImage mapping. All callbacks execute in UEFI context.
 * Never returns a native-ready certificate. NULL callbacks refuse all SPI calls. */
int sm8750_first_read(efi_boot_services *,int enabled,int running_live,
 const void *image,size_t image_size,int (*prepare)(void *),int (*release)(void *),
 void *panel,struct sm8750_first_read_session *,struct sm8750_first_read_report *);
int sm8750_first_read_cleanup(efi_boot_services *,int (*release)(void *),void *,
 struct sm8750_first_read_session *,struct sm8750_first_read_report *);
#endif
