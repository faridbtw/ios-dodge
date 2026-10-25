/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef FIFO_SNAPSHOT_H
#define FIFO_SNAPSHOT_H
#include <stdint.h>
#include <stddef.h>
#define FIFO_SNAPSHOT_COUNT 16
struct fifo_snapshot {
 uint32_t first[FIFO_SNAPSHOT_COUNT],second[FIFO_SNAPSHOT_COUNT];
 uint32_t completed,stable,local_idle,protocol_spi;
 int result;
};
/* Caller retains clock ownership until this returns. No writes exist in this
 * API. callback validates AT/device mapping and fixed register addresses. */
int fifo_snapshot_read(int enabled,int (*read32)(void *,uint64_t,uint32_t *),
 void *,struct fifo_snapshot *);
uint32_t fifo_snapshot_offset(unsigned index);
#endif
