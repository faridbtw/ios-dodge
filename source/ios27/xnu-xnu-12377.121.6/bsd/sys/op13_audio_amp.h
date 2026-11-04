/* SPDX-License-Identifier: BSD-2-Clause
 * AW88461/PID2308 native profile preparation with powerdown and mute held.
 * Caller owns reset and on every error must assert reset before releasing
 * the GPIO lease. Start requires native profile, attenuation and healthy clocks.
 */
#ifndef OP13_AUDIO_AMP_H
#define OP13_AUDIO_AMP_H
#include "op13_audio_amp_profile.h"
struct op13_aw_io {
 void *context;
 int (*read)(void *,uint8_t,uint16_t *);
 int (*write)(void *,uint8_t,uint16_t);
};
static int op13_aw_register(unsigned reg)
{return (reg>=3&&reg<=0x1f)||(reg>=0x30&&reg<=0x3f&&reg!=0x39)||(reg>=0x50&&reg<=0x6d);}
static int op13_aw_prepare_muted(const struct op13_aw_io *io,const struct op13_aw_profile *p)
{
 if(!io||!io->read||!io->write||!p||!p->registers||!p->count||p->count>128)return -1;
 unsigned controls=0;
 for(unsigned i=0;i<p->count;i++){
  unsigned reg=op13_aw_u16(p->registers+4*i),v=op13_aw_u16(p->registers+4*i+2);
  if(reg==0xff&&!v&&i==p->count-1)continue;
  if(!op13_aw_register(reg))return -1;
  if(reg==4)controls|=1;
  if(reg==5)controls|=2;
 }
 if(controls!=3)return -1;
 uint16_t id=0,ctrl=0,volume=0;
 if(io->read(io->context,0,&id)||id!=0x2308)return -2;
 if(io->read(io->context,4,&ctrl)||io->read(io->context,5,&volume))return -3;
 /* PID2308: PWDN0, AMPPD1, HMUTE8, ULS_HMUTE15, I2STXEN13.
  * Its volume is attenuation in0.0940625dB steps;384 adds36.12dB.
  */
 if(io->write(io->context,4,(ctrl|0x8103u)&~0x2000u)||
    io->write(io->context,5,(volume&0xfc00u)|0x3ffu))return -3;
 for(unsigned i=0;i<p->count;i++){
  unsigned reg=op13_aw_u16(p->registers+4*i),v=op13_aw_u16(p->registers+4*i+2);
  if(reg==0xff)continue;
  if(reg==3)v=0xffff; /* Polling test: mask amplifier interrupts. */
  if(reg==4)v=(v|0x8103u)&~0x2000u;
  if(reg==5){unsigned attenuation=(v&0x3ffu)+384u;if(attenuation>1023)attenuation=1023;v=(v&0xfc00u)|attenuation;}
  if(io->write(io->context,(uint8_t)reg,(uint16_t)v))return -3;
 }
 if(io->read(io->context,4,&ctrl)||io->read(io->context,5,&volume)||
    (ctrl&0xa103)!=0x8103||(volume&0x3ffu)<384)return -4;
 return 0;
}
static int op13_aw_start_quiet(const struct op13_aw_io *io,
 void (*delay_us)(void *,unsigned),void *delay_context)
{
 if(!io||!io->read||!io->write||!delay_us)return -1;
 uint16_t ctrl=0,volume=0,status=0;
 if(io->read(io->context,4,&ctrl)||io->read(io->context,5,&volume)||
    (ctrl&0xa103)!=0x8103||(volume&0x3ff)<384)return -1;
 /* PWDN off, with amplifier powerdown and both mutes still asserted. */
 ctrl&=~1u;if(io->write(io->context,4,ctrl))return -2;
 delay_us(delay_context,2000);
 unsigned n;for(n=0;n<10;n++){
  if(io->read(io->context,1,&status))return -2;
  if((status&0x11)==0x11&&!(status&0x482a))break;
  delay_us(delay_context,2000);
 }
 if(n==10)return -3;
 ctrl&=~2u;if(io->write(io->context,4,ctrl))return -2;
 delay_us(delay_context,1000);
 for(n=0;n<10;n++){
  if(io->read(io->context,1,&status))return -2;
  if((status&0x211)==0x211&&!(status&0x482a))break;
  delay_us(delay_context,2000);
 }
 if(n==10)return -3;
 /* Recheck attenuation immediately before removing mute. */
 if(io->read(io->context,5,&volume)||(volume&0x3ff)<384)return -1;
 ctrl&=~0x8100u;
 if(io->write(io->context,4,ctrl)||io->read(io->context,4,&ctrl)||
    (ctrl&0xa103))return -2;
 return 0;
}
#endif
