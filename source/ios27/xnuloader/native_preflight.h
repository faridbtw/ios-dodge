#ifndef OP13_NATIVE_PREFLIGHT_H
#define OP13_NATIVE_PREFLIGHT_H
#include <stdint.h>
struct native_snapshot {uint32_t fw,status,tx,rx,m_irq,s_irq,route,gsi;};
struct native_lease {uint64_t generation,base;unsigned closed,clock_refs,power_refs,single_cpu,no_reentry;};
int native_preflight(const struct native_lease *,uint64_t,int (*)(void*,uint32_t,uint32_t*),void *,struct native_snapshot *);
#endif
