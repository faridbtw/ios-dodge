/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Mode-0 loop adapted from tycykp/UniXNU-OnePlus-Ace5 giulia_touch_gpio.h,
 * commit18f51ec652501c62e19dd4f1035dc51daa2a3361, and the HackiOS GPIO SPI
 * fallback. OnePlus13 wiring is independently pinned in touch/REFERENCES.md.
 * Fallible callbacks, snapshot ownership and restoration are local additions.
 */
/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef OP13_BITBANG_H
#define OP13_BITBANG_H
#include <stdint.h>
#include <stddef.h>
struct op13_bitbang_io {
 void *cookie;
 int (*read)(void *,uint32_t,uint32_t *);
 int (*write)(void *,uint32_t,uint32_t);
 int (*delay)(void *,unsigned);
};
struct op13_bitbang {
 struct op13_bitbang_io io;
 uint32_t saved_cfg[4],saved_io[4];
 unsigned owned,ready,poisoned;
};
int op13_bitbang_open(struct op13_bitbang *,const struct op13_bitbang_io *,int);
int op13_bitbang_transfer(struct op13_bitbang *,const uint8_t *,uint8_t *,size_t);
/* Caller holds panel reset low and supplies until this succeeds. */
int op13_bitbang_cleanup(struct op13_bitbang *);
#endif

static uint32_t off(unsigned p,unsigned r){return (48u+p)*0x1000u+r;}
static int rd(struct op13_bitbang *s,unsigned p,unsigned r,uint32_t *v)
{return s->io.read(s->io.cookie,off(p,r),v);}
static int wr(struct op13_bitbang *s,unsigned p,unsigned r,uint32_t v,uint32_t mask)
{
 uint32_t got;
 if(s->io.write(s->io.cookie,off(p,r),v)||rd(s,p,r,&got))return -1;
 return ((v^got)&mask)?-1:0;
}
static int level(struct op13_bitbang *s,unsigned p,unsigned high)
{
 uint32_t v;
 if(p==0||rd(s,p,4,&v))return -1;
 return wr(s,p,4,(v&~2u)|(high?2u:0),2);
}
static int config(struct op13_bitbang *s,unsigned p,unsigned output)
{
 static const uint32_t drive[4]={0x80,0x40,0x80,0x40};
 return wr(s,p,0,(s->saved_cfg[p]&~0x3ffu)|drive[p]|(output?0x200u:0),0x3ff);
}
int op13_bitbang_open(struct op13_bitbang *s,const struct op13_bitbang_io *io,int enabled)
{
 if(!s||!io)return -5;
 if(!enabled)return -3;
 if(s->owned)return -6;
 if(!io->read||!io->write||!io->delay)return -3;
 s->io=*io;s->ready=s->poisoned=0;
 for(unsigned i=0;i<4;i++){
  if(rd(s,i,0,&s->saved_cfg[i])||rd(s,i,4,&s->saved_io[i]))return -1;
  if(s->saved_cfg[i]&0x1800u)return -3;
 }
 /* A failed write may already have taken effect. Claim BEFORE first write. */
 s->owned=1;
 if(level(s,3,1)||config(s,3,1)||config(s,0,0)||
    level(s,1,1)||config(s,1,1)||level(s,2,0)||config(s,2,1))return -1;
 s->ready=1;return 0;
}
int op13_bitbang_transfer(struct op13_bitbang *s,const uint8_t *tx,uint8_t *rx,size_t n)
{
 if(!s||!s->ready||s->poisoned)return -6;
 if(!n||n>1032||(!tx&&!rx))return -5;
 if(level(s,2,0)||level(s,3,0)||s->io.delay(s->io.cookie,5))goto fail;
 for(size_t i=0;i<n;i++){
  uint8_t out=tx?tx[i]:255,in=0;
  for(unsigned bit=0;bit<8;bit++){
   uint32_t sample;
   if(level(s,1,(out>>(7-bit))&1)||s->io.delay(s->io.cookie,5)||
      level(s,2,1)||s->io.delay(s->io.cookie,5)||rd(s,0,4,&sample)||
      level(s,2,0))goto fail;
   in=(uint8_t)((in<<1)|(sample&1));
  }
  if(rx)rx[i]=in;
 }
 if(s->io.delay(s->io.cookie,5)||level(s,3,1)||s->io.delay(s->io.cookie,5))goto fail;
 return 0;
fail:
 s->ready=0;s->poisoned=1;
 (void)level(s,3,1);
 return -1;
}
int op13_bitbang_cleanup(struct op13_bitbang *s)
{
 if(!s)return -5;
 if(!s->owned)return 0;
 s->ready=0;
 /* Reset is held low by caller. Deassert CS before making all pins high-Z.
  * Restore latches only while input, then restore the original configurations.
  * Do not touch MISO's latch: it was never written by this transport. */
 if(level(s,3,1))return -1;
 for(unsigned i=0;i<4;i++){
  uint32_t v;
  if(rd(s,i,0,&v)||wr(s,i,0,(v&~0x3fcu),0x3fc))return -1;
 }
 for(unsigned i=1;i<4;i++){
  uint32_t v;
  if(rd(s,i,4,&v)||wr(s,i,4,(v&~2u)|(s->saved_io[i]&2u),2))return -1;
 }
 for(unsigned i=0;i<4;i++)if(wr(s,i,0,s->saved_cfg[i],0x3ff))return -1;
 s->owned=0;return 0;
}

/* SPDX-License-Identifier: BSD-2-Clause
 * GPIO-only board binding. Supplies/reset/pin snapshots belong to the resident
 * loader, which retains them until a confirmed kernel return. No GENI access.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_s3910.h>
#include <sys/systm.h>
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
static vm_offset_t gpio_bank;
static struct op13_bitbang gpio_bus;
static int gpio_allowed(uint32_t off)
{
 unsigned p=off/0x1000,r=off%0x1000;
 return p>=48&&p<=51&&(r==0||r==4);
}
static int gpio_read(void *cookie,uint32_t off,uint32_t *v)
{
 (void)cookie;if(!gpio_bank||!v||!gpio_allowed(off))return -5;
 __asm__ volatile("dsb sy" ::: "memory");
 *v=*(volatile uint32_t *)(gpio_bank+off);
 __asm__ volatile("dsb sy" ::: "memory");return 0;
}
static int gpio_write(void *cookie,uint32_t off,uint32_t v)
{
 (void)cookie;if(!gpio_bank||!gpio_allowed(off)||off==48*0x1000+4)return -5;
 __asm__ volatile("dsb sy" ::: "memory");
 *(volatile uint32_t *)(gpio_bank+off)=v;
 __asm__ volatile("dsb sy" ::: "memory");return 0;
}
static int gpio_delay(void *cookie,unsigned us)
{
 (void)cookie;if(us>1000000)return -5;
 delay_for_interval(us,NSEC_PER_USEC);return 0;
}
static int gpio_bus_read(void *cookie,uint8_t *p,size_t n)
{return op13_bitbang_transfer(cookie,0,p,n);}
static int gpio_bus_write(void *cookie,const uint8_t *p,size_t n)
{return op13_bitbang_transfer(cookie,p,0,n);}
static int gpio_irq(void *cookie)
{
 (void)cookie;if(!gpio_bus.ready||!gpio_bank)return -6;
 uint32_t v=*(volatile uint32_t *)(gpio_bank+162*0x1000+4);
 __asm__ volatile("dsb sy" ::: "memory");return !(v&1);
}
static void gpio_sleep(void *cookie,unsigned ms)
{(void)cookie;while(ms--)delay_for_interval(1000,NSEC_PER_USEC);}
int sm8750_touch_bus_open(struct s3910_io *io)
{
 int enabled=0;uint64_t pa=0;
 if(!io)return S3910_RANGE;
 PE_parse_boot_argn("sm8750_touch_hw",&enabled,sizeof(enabled));
 if(enabled!=1||!PE_parse_boot_argn("touch_lease_pa",&pa,sizeof(pa))||
    pa<0x80000000ULL||pa>=0x1000000000ULL||(pa&0x3fff)){
  printf("sm8750-touch: no loader GPIO lease; no hardware writes\n");return S3910_UNSUPPORTED;
 }
 vm_offset_t lease=ml_io_map((vm_offset_t)pa,0x4000);
 if(!lease)return S3910_IO;
 static const uint32_t expected[12]={0x4f503133,1,48,0x0f100000,0x100000,48,49,50,51,161,162,5};
 for(unsigned i=0;i<12;i++)if(((volatile uint32_t *)lease)[i]!=expected[i]){
  printf("sm8750-touch: invalid immutable lease field %u; no GPIO writes\n",i);
  return S3910_UNSUPPORTED;
 }
 gpio_bank=ml_io_map(0x0f100000,0x100000);
 if(!gpio_bank)return S3910_IO;
 static const uint32_t expected_cfg[4]={0x80,0x240,0x280,0x240};
 for(unsigned i=0;i<4;i++){
  uint32_t v;if(gpio_read(0,(48+i)*0x1000,&v)||
    (v&0x3ff)!=expected_cfg[i]||(v&0x1800)){
   printf("sm8750-touch: leased GPIO%u configuration changed; no writes\n",48+i);
   return S3910_NOT_READY;
  }
 }
 gpio_bus.io=(struct op13_bitbang_io){0,gpio_read,gpio_write,gpio_delay};
 gpio_bus.ready=1;gpio_bus.poisoned=0; /* loader owns all snapshots and cleanup */
 io->cookie=&gpio_bus;io->read=gpio_bus_read;io->write=gpio_bus_write;
 io->irq_asserted=gpio_irq;io->sleep_ms=gpio_sleep;
 printf("sm8750-touch: loader lease verified; mode0 GPIO SPI, 5us half-cycle, IRQ162 active-low\n");
 return 0;
}
void sm8750_touch_bus_close(void)
{
 gpio_bus.ready=0;
 /* No power/pin rollback while the kernel still owns its execution core. */
 printf("sm8750-touch: worker stopped; loader retains supplies/pin snapshots until kernel return\n");
}
