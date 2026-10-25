/* SPDX-License-Identifier: BSD-2-Clause */
#include "sm8750_panel.h"
static int allowed(uint32_t o)
{ return o==0xa1000||o==0xa1004||o==0xa2000||o==0xa2004; }
int sm8750_panel_raw_read(void *cookie,uint32_t off,uint32_t *value)
{
 (void)cookie;
 if(!value||!allowed(off)) return -5;
#ifdef __aarch64__
 __asm__ volatile("dsb sy" ::: "memory");
 *value=*(volatile uint32_t *)(uintptr_t)(0xf100000u+off);
 __asm__ volatile("dsb sy" ::: "memory");
 return 0;
#else
 return -3;
#endif
}
int sm8750_panel_raw_write(void *cookie,uint32_t off,uint32_t value)
{
 (void)cookie;
 /* Never write IRQ's I/O or any interrupt-controller registers. */
 if(!allowed(off)||off==0xa2004) return -5;
#ifdef __aarch64__
 __asm__ volatile("dsb sy" ::: "memory");
 *(volatile uint32_t *)(uintptr_t)(0xf100000u+off)=value;
 __asm__ volatile("dsb sy" ::: "memory");
 return 0;
#else
 (void)value; return -3;
#endif
}
