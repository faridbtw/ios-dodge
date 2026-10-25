/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef SM8750_CONFIG_QUERY_H
#define SM8750_CONFIG_QUERY_H
#include "efi.h"
struct sm8750_config_query_report {
 uint64_t locate_status,core_base,common_base;
 uint64_t se_wrapper_base,derived_base,wrapper_common_base;
 int32_t extended_status[3];
 uint8_t se_index;
 uint8_t extended_reserved[3];
 uint32_t derived_matched_a90000;
 int32_t path_status[3],value_status[4];
 uint8_t count0,count1,reserved[6];
 uint32_t logical_instance,matched_a90000;
};
/* Separate explicit opt-in from passive SPI inspection. Calls only the three
 * configuration-tree lookup/getter slots observed in the pinned SPIDxe; no
 * SPI open, MMIO access, power, firmware load or bus transfer. Requires the
 * caller first establish the exact baseline firmware (not just protocol GUID).
 */
int sm8750_loader_config_query(efi_boot_services *,int enabled,
 struct sm8750_config_query_report *);
#endif
