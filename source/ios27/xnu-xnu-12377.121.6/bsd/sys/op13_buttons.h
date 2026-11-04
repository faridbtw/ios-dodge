/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef OP13_BUTTONS_H
#define OP13_BUTTONS_H
#include <stdint.h>
#include <stddef.h>
enum op13_region { OP13_CORE, OP13_CNFG, OP13_OBSERVER };
enum op13_error { OP13_OK=0, OP13_IO=-1, OP13_UNSUPPORTED=-3,
 OP13_TIMEOUT=-4, OP13_RANGE=-5, OP13_NOT_READY=-6, OP13_DENIED=-8 };
enum op13_key { OP13_POWER=0, OP13_VOLUP=1, OP13_VOLDOWN=2 };
/* Mapping and bounds are fixed by this board's captured DT. */
#define OP13_CORE_PA 0x0c400000u
#define OP13_CORE_SIZE 0x3000u
#define OP13_CNFG_PA 0x0c42d000u
#define OP13_CNFG_SIZE 0x4000u
#define OP13_OBSERVER_PA 0x0c440000u
#define OP13_OBSERVER_SIZE 0x80000u
#define OP13_DEBOUNCE_US 32000u

struct op13_host {
 void *cookie;
 uint32_t (*read32)(void *, enum op13_region, uint32_t);
 /* The only write operation: native observer read-command register. No PMIC
  * write, non-data command, reset, SPMI channel or IRQ write is exposed. */
 void (*issue_read)(void *, uint32_t observer_offset, uint32_t command);
 void (*lock)(void *);
 void (*unlock)(void *);
 void (*delay_us)(void *, unsigned);
 uint64_t (*time_us)(void *);
 void (*event)(void *, unsigned key, unsigned pressed);
 void (*diagnostic)(void *, const char *, uint32_t, uint32_t);
};
struct op13_key_state {
 uint64_t candidate_since;
 uint8_t candidate, stable, armed, sampled;
};
struct op13_buttons {
 struct op13_host host;
 struct op13_key_state keys[3];
 uint32_t version, count, observer[2], last_status, transactions;
 uint16_t apid[2];
 uint8_t owner[2], stage, poisoned;
};
/* Run stages in order, under a single worker. Stage1/2 never write MMIO. */
int op13_version(struct op13_buttons *, const struct op13_host *);
int op13_find_apids(struct op13_buttons *);
int op13_identify(struct op13_buttons *);
int op13_validate_inputs(struct op13_buttons *);
int op13_poll(struct op13_buttons *);
void op13_stop(struct op13_buttons *);
/* Restricted diagnostic reader: only the two button peripherals and a fixed
 * read-only address allowlist. Not a general PMIC register access interface. */
int op13_read(struct op13_buttons *, unsigned device, unsigned offset, uint8_t *, size_t);
#endif
