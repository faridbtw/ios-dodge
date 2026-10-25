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
