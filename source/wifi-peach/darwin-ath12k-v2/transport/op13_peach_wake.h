/* SPDX-License-Identifier: BSD-2-Clause
 * Native Peach hif_soc_wake_request: if_pci.h/if_pci.c, pinned in UPSTREAM.json.
 * Caller serializes PCI window selection and owns an active MHI wake vote.
 */
#ifndef OP13_PEACH_WAKE_H
#define OP13_PEACH_WAKE_H
#include <stdint.h>
struct op13_peach_wake_ops {
 uint32_t (*read)(void *, unsigned);
 void (*write)(void *, unsigned, uint32_t);
 void (*pause_ms)(void *, unsigned);
};
struct op13_peach_wake_result { uint32_t before, after; unsigned polls; };
static int op13_peach_wake_umac(const struct op13_peach_wake_ops *ops, void *ctx,
 struct op13_peach_wake_result *result)
{
 if (!ops || !ops->read || !ops->write || !ops->pause_ms || !result) return -1;
 *result=(struct op13_peach_wake_result){0};
 uint32_t saved=ops->read(ctx,0x3278);
 if(saved==~0u)return -2;
 uint32_t selected=(saved&~0x3fu)|0x40000000u|0x3cu;
 int rc=0;
 ops->write(ctx,0x3004,1);
 ops->write(ctx,0x3278,selected);
 if(ops->read(ctx,0x3278)!=selected){rc=-3;goto restore;}
 result->before=ops->read(ctx,0x84040); /* native 0x01e04040, dynamic window */
 result->after=result->before;
 for(unsigned waited=0;;waited+=5){
  result->polls++;
  if(result->after==2)break;
  if(waited>=50){rc=-4;break;}
  ops->pause_ms(ctx,5);
  result->after=ops->read(ctx,0x84040);
 }
restore:
 ops->write(ctx,0x3278,saved);
 if(ops->read(ctx,0x3278)!=saved)rc=-5;
 if(rc)ops->write(ctx,0x3004,0);
 /* Successful wake stays held until the exclusive RAM lease is stopped. */
 return rc;
}
#endif
