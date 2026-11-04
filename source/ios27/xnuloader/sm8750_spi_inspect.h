/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef SM8750_SPI_INSPECT_H
#define SM8750_SPI_INSPECT_H
#include "efi.h"
struct sm8750_spi_topology {
 uint64_t revision;
 uint32_t image_size,initialized,logical_instance;
 uint8_t wrapper_counts[4];
 uint32_t physical_address_verified; /* remains zero: DAL base not established */
};
/* Supply an EFI_LOADED_IMAGE mapping, NOT a guessed pointer near a function.
 * Locates SPI protocol, validates pinned layout/code, reads RAM tables only.
 * Never invokes SPI open/transfer/close or its internal DAL initializer.
 * Return0 means a logical TOP_QUP1/SE4 mapping exists; NOT permission to open.
 * -6 means table not initialized, -3 unsupported image/ABI, -5 bad parameters.
 */
int sm8750_loader_spi_inspect(efi_boot_services *,int enabled,
 const void *image_base,size_t mapped_size,struct sm8750_spi_topology *);
struct sm8750_config_protocol_report {
 uint64_t locate_status,interface_address;
 uint64_t slots[41]; /* exact 0x148-byte copy published by pinned EnvDxe */
};
/* Observes the published interface only; calls none of its methods. Parent
 * should correlate pointer slots with EFI memory-map ranges, not dereference
 * function addresses or invoke unknown signatures. */
int sm8750_loader_config_inspect(efi_boot_services *,int enabled,
 struct sm8750_config_protocol_report *);
#endif
