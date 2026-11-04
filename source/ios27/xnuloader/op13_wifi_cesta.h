/* SPDX-License-Identifier: BSD-2-Clause
 * Bounded PCIe CRM-v2 software vote, based on vendor register/ABI definitions.
 * Only root0 SW-DRV0 ACTIVE performance is changed; HW channels and bandwidth
 * votes are untouched. Gen1=2 follows the stock PCIe performance-level enum.
 */
#ifndef OP13_WIFI_CESTA_H
#define OP13_WIFI_CESTA_H
#include <stdint.h>
struct op13_cesta_io {
 void *context;
 uint32_t (*read)(void *,uint32_t);
 void (*write)(void *,uint32_t,uint32_t);
 void (*delay_us)(void *,unsigned);
};
enum {
 OP13_CRM_SW_ACTIVE=0x3b0, OP13_CRM_SW_IRQ=0x3c0, OP13_CRM_SW_IRQ_CLEAR=0x3c4,
 OP13_CRM_COMMON_VERSION=0x4f00, OP13_CRM_COMMON_CONFIG=0x4f04,
 OP13_CRM_COMMON_ENABLE=0x4f0c, OP13_CRM_CURRENT_PERF=0x2824
};
static inline int
op13_cesta_gen1_vote(const struct op13_cesta_io *io,uint32_t *saved)
{
 if(!io||!io->read||!io->write||!io->delay_us||!saved) return -1;
 void *c=io->context;
 if(io->read(c,OP13_CRM_COMMON_VERSION)!=0x20000 ||
    io->read(c,OP13_CRM_COMMON_CONFIG)!=0x80228225 ||
    !(io->read(c,OP13_CRM_COMMON_ENABLE)&1)) return -1;
 uint32_t old=io->read(c,OP13_CRM_SW_ACTIVE);
 /* Only claim an idle software vote, with no pending completion to consume. */
 if(old || io->read(c,OP13_CRM_SW_IRQ) || (io->read(c,OP13_CRM_CURRENT_PERF)&7)) return -2;
 *saved=old;
 io->write(c,OP13_CRM_SW_ACTIVE,2);
 for(unsigned i=0;i<10000;i++) {
  uint32_t v=io->read(c,OP13_CRM_CURRENT_PERF)&7;
  if(v>=2 && v<=5) return 0;
  io->delay_us(c,10);
 }
 return -3; /* Vote may be live: retain resources and cold-reset. */
}
static inline int
op13_cesta_restore(const struct op13_cesta_io *io,uint32_t saved)
{
 if(!io||!io->read||!io->write||!io->delay_us||saved) return -1;
 io->write(io->context,OP13_CRM_SW_ACTIVE,saved);
 for(unsigned i=0;i<10000;i++) {
  if((io->read(io->context,OP13_CRM_CURRENT_PERF)&7)==saved) {
   /* Initial IRQ state was zero; clear only our resource0 completion. */
   io->write(io->context,OP13_CRM_SW_IRQ_CLEAR,1);
   return io->read(io->context,OP13_CRM_SW_IRQ)==0 ? 0 : -3;
  }
  io->delay_us(io->context,10);
 }
 return -3;
}
#endif
