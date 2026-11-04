#ifndef OP13_BATTERY_DSP_H
#define OP13_BATTERY_DSP_H
#include <kern/locks.h>
#include <mach/mach_types.h>
extern lck_mtx_t op13_batt_aop_mailbox_lock;
extern lck_mtx_t op13_batt_smem_alloc_lock;
extern lck_mtx_t op13_batt_rpmh_lock;
uint32_t op13_batt_ipcc_take(vm_offset_t base, uint32_t client);
void op13_batt_adsp_init(void);
void op13_batt_adsp_stop(void);
void op13_batt_adsp_quiesce(void);
const uint8_t *op13_batt_image_file(const char *, const char *, uint64_t *);
#endif
