/* Bounded AK09970 identity/data test; no PMIC/IRQ/storage writes. */
#include "op13_slider_i2c.h"
#include "op13_slider_power.inc"
static const uint64_t op13_slider_gpio[]={0x0f150000,0x0f151000};
static uint32_t op13_slider_read(void *c,unsigned p,unsigned o)
{(void)c;return *(volatile uint32_t *)(uintptr_t)(op13_slider_gpio[p]+o);}
static void op13_slider_write(void *c,unsigned p,unsigned o,uint32_t v)
{(void)c;*(volatile uint32_t *)(uintptr_t)(op13_slider_gpio[p]+o)=v;__asm__ volatile("dsb sy":::"memory");}
static void op13_slider_delay(void *c,unsigned us){(void)c;BS->stall(us);}
static int op13_slider_probe(void)
{
 int power=op13_slider_power();if(power)return power;
 for(unsigned p=0;p<2;p++) {
  uint64_t par=uefi_page_translation(op13_slider_gpio[p]);
  if((par&1)||((par&0x000ffffffffff000ULL)!=op13_slider_gpio[p])) {
   print("op13-slider: GPIO%d unmapped PAR=%x\n",(uint64_t)(80+p),par);return -5;
  }
 }
 struct op13_slider_bus b={0,op13_slider_read,op13_slider_write,op13_slider_delay,{0},{0},0};
 print("op13-slider: original SDA80 cfg=%x SCL81 cfg=%x\n",(uint64_t)op13_slider_read(0,0,0),(uint64_t)op13_slider_read(0,1,0));
 uint8_t id[2]={0},mode=0;int r=op13_slider_transfer(&b,0,id,2,0);
 print("op13-slider: ID result=%d bytes=%x %x\n",(uint64_t)(int64_t)r,(uint64_t)id[0],(uint64_t)id[1]);
 if(r||id[0]!=0x48||id[1]!=0xc1)return -6;
 r=op13_slider_transfer(&b,0x21,&mode,1,0);
 print("op13-slider: current mode=%x read-status=%d\n",(uint64_t)mode,(uint64_t)(int64_t)r);
 if(r)return r;
 uint8_t measure=0x2a; /* exact vendor continuous low-power mode */
 r=op13_slider_transfer(&b,0x21,&measure,1,1);
 if(!r) {
  BS->stall(50000);
  uint8_t xyz[7]={0};r=op13_slider_transfer(&b,0x17,xyz,7,0);
  print("op13-slider: data result=%d status=%x x=%d y=%d z=%d\n",(uint64_t)(int64_t)r,(uint64_t)xyz[0],
   (uint64_t)(int64_t)(int16_t)((xyz[5]<<8)|xyz[6]),(uint64_t)(int64_t)(int16_t)((xyz[3]<<8)|xyz[4]),
   (uint64_t)(int64_t)(int16_t)((xyz[1]<<8)|xyz[2]));
 }
 int restore=op13_slider_transfer(&b,0x21,&mode,1,1);
 print("op13-slider: mode restore=%d original muxes restored after each transfer\n",(uint64_t)(int64_t)restore);
 return r?r:restore;
}
