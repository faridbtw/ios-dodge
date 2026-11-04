#include <stdint.h>
#include <stddef.h>
/* Read-only preflight, not a ready producer. Supply ordered device reads only
 * after loader proves retained resources and the exact mapping. */
#include "native_preflight.h"
int native_preflight(const struct native_lease *l,uint64_t generation,
 int (*read)(void*,uint32_t,uint32_t*),void *cookie,struct native_snapshot *s)
{
 static const uint32_t offsets[]={0x68,0x40,0x800,0x804,0x614,0x644,0xe1c,0xe18};
 if(!l||!s||!read||!generation||l->generation!=generation||l->base!=0xa90000||
 !l->closed||!l->clock_refs||!l->power_refs||!l->single_cpu||!l->no_reentry)return -3;
 uint32_t v[8];for(unsigned i=0;i<8;i++)if(read(cookie,offsets[i],&v[i]))return -1;
 *s=(struct native_snapshot){v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7]};
 /* SPI protocol1, both sequencers inactive, FIFO empty, IRQ masks zero.
  * Counts use conservative entire low28 bits; unknown flags fail closed. */
 if(((s->fw>>8)&0xff)!=1||(s->status&((1u<<0)|(1u<<12)))||
 (s->tx&0x0fffffff)||(s->rx&0x0fffffff)||s->m_irq||s->s_irq||s->route||s->gsi)return -6;
 return 0; /* Snapshot accepted; caller still must verify lease provenance. */
}
