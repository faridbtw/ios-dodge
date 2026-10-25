/* SPDX-License-Identifier: BSD-2-Clause
 * Dodge AK09970, DT i2c@9a4000: SDA80/SCL81, address0x10.
 * Open drain only. Every transfer restores the two original GPIO muxes.
 * Inspired by haxi0 instantnoodle_i2c.c; hardware profile is Sun, not SM8250.
 */
#ifndef OP13_SLIDER_I2C_H
#define OP13_SLIDER_I2C_H
#include <stdint.h>
struct op13_slider_bus {
 void *cookie;
 uint32_t (*read)(void *, unsigned, unsigned);
 void (*write)(void *, unsigned, unsigned, uint32_t);
 void (*delay)(void *, unsigned);
 uint32_t saved_cfg[2],saved_data[2];
 int fault;
};
enum { OP13_SLIDER_SDA=0, OP13_SLIDER_SCL=1 };
static void op13_slider_line(struct op13_slider_bus *b,unsigned pin,int high)
{
 /* Preserve upper vendor fields; GPIO mode, pull up,2mA; never drive high. */
 if(!high)b->write(b->cookie,pin,4,b->saved_data[pin]&~2u);
 b->write(b->cookie,pin,0,(b->saved_cfg[pin]&~0x3ffu)|3u|0x2000u|(high?0:0x200u));
}
static int op13_slider_high(struct op13_slider_bus *b)
{
 op13_slider_line(b,OP13_SLIDER_SCL,1);
 for(unsigned i=0;i<1000;i++) {
  if(b->read(b->cookie,OP13_SLIDER_SCL,4)&1)return 0;
  b->delay(b->cookie,1);
 }
 b->fault=-2;return -2;
}
static void op13_slider_pause(struct op13_slider_bus *b){b->delay(b->cookie,4);}
static int op13_slider_start(struct op13_slider_bus *b)
{
 op13_slider_line(b,0,1);
 if(op13_slider_high(b))return b->fault;
 op13_slider_pause(b);
 if(!(b->read(b->cookie,0,4)&1)){b->fault=-3;return -3;}
 op13_slider_line(b,0,0);op13_slider_pause(b);
 op13_slider_line(b,1,0);op13_slider_pause(b);return 0;
}
static void op13_slider_stop(struct op13_slider_bus *b)
{
 op13_slider_line(b,0,0);op13_slider_pause(b);
 op13_slider_high(b);op13_slider_pause(b);
 op13_slider_line(b,0,1);op13_slider_pause(b);
}
static int op13_slider_tx(struct op13_slider_bus *b,uint8_t byte)
{
 for(int i=7;i>=0;i--) {
  op13_slider_line(b,0,(byte>>i)&1);op13_slider_pause(b);
  if(op13_slider_high(b))return b->fault;
  op13_slider_pause(b);op13_slider_line(b,1,0);op13_slider_pause(b);
 }
 op13_slider_line(b,0,1);op13_slider_pause(b);
 if(op13_slider_high(b))return b->fault;
 op13_slider_pause(b);
 int nack=b->read(b->cookie,0,4)&1;
 op13_slider_line(b,1,0);op13_slider_pause(b);
 return nack?-1:0;
}
static int op13_slider_rx(struct op13_slider_bus *b,uint8_t *value,int ack)
{
 uint8_t v=0;op13_slider_line(b,0,1);
 for(unsigned i=0;i<8;i++) {
  op13_slider_pause(b);if(op13_slider_high(b))return b->fault;
  op13_slider_pause(b);v=(uint8_t)((v<<1)|(b->read(b->cookie,0,4)&1));
  op13_slider_line(b,1,0);
 }
 op13_slider_line(b,0,!ack);op13_slider_pause(b);
 if(op13_slider_high(b))return b->fault;
 op13_slider_pause(b);op13_slider_line(b,1,0);op13_slider_line(b,0,1);
 *value=v;return 0;
}
static int op13_slider_transfer(struct op13_slider_bus *b,uint8_t reg,uint8_t *data,unsigned n,int writing)
{
 if(!data||n==0||n>8)return -4;
 b->fault=0;
 for(unsigned p=0;p<2;p++) {
  b->saved_cfg[p]=b->read(b->cookie,p,0);
  b->saved_data[p]=b->read(b->cookie,p,4);
 }
 int r=op13_slider_start(b);
 if(!r)r=op13_slider_tx(b,0x20);
 if(!r)r=op13_slider_tx(b,reg);
 if(!writing) {
  if(!r)r=op13_slider_start(b);
  if(!r)r=op13_slider_tx(b,0x21);
 }
 for(unsigned i=0;!r&&i<n;i++)r=writing?op13_slider_tx(b,data[i]):op13_slider_rx(b,&data[i],i+1<n);
 op13_slider_stop(b);
 if(!r&&b->fault)r=b->fault;
 for(unsigned p=0;p<2;p++) {
  /* Disable output before restoring the original latch and mux. */
  b->write(b->cookie,p,0,b->saved_cfg[p]&~0x200u);
  b->write(b->cookie,p,4,b->saved_data[p]);
  b->write(b->cookie,p,0,b->saved_cfg[p]);
 }
 return r;
}
#endif
