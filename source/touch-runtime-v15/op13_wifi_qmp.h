/* SPDX-License-Identifier: BSD-2-Clause
 * Bounded AOSS-QMP sender for the exact WLAN PDC initialization from phone DT.
 * Protocol layout matches native battery's already validated AOP mailbox path.
 * Does not reset an existing link or clear an unacknowledged message.
 */
#ifndef OP13_WIFI_QMP_H
#define OP13_WIFI_QMP_H
#include <stdint.h>
struct op13_wifi_qmp_io {
 void *context;
 uint32_t (*read)(void *,uint32_t);
 void (*write)(void *,uint32_t,uint32_t);
 void (*ring)(void *);
 void (*delay)(void *,unsigned);
};
static const char *const op13_wifi_pdc_messages[]={
 "{class: wlan_pdc, ss: rf, res: s5f.m, enable: 1}",
 "{class: wlan_pdc, ss: rf, res: s5f.v, enable: 1}",
 "{class: wlan_pdc, ss: rf, res: s5f.v, upval: 876}",
 "{class: wlan_pdc, ss: rf, res: s5f.v, dwnval: 876}"
};
static int op13_wifi_qmp_wait(const struct op13_wifi_qmp_io *io,uint32_t reg,uint32_t value)
{
 for(unsigned i=0;i<10000;i++){
  if(io->read(io->context,reg)==value)return 0;
  io->delay(io->context,100);
 }
 return -1;
}
static int op13_wifi_qmp_box(const struct op13_wifi_qmp_io *io,uint32_t *box)
{
 if(io->read(io->context,0)!=0x4d41494c||io->read(io->context,4)!=1)return -1;
 uint32_t at=io->read(io->context,0x38),size=io->read(io->context,0x34);
 if((at&3)||at<0x3c||at>0x3bc||size<68||size>0x400-at)return -2;
 *box=at;return 0;
}
static int op13_wifi_qmp_open(const struct op13_wifi_qmp_io *io,uint32_t *box)
{
 if(op13_wifi_qmp_box(io,box))return -1;
 if(io->read(io->context,0x24)==0xffff&&io->read(io->context,0x28)==0xffff&&
    io->read(io->context,0x2c)==0xffff&&io->read(io->context,0x14)==0xffff&&
    io->read(io->context,0x30)==0xffff)return 0;
 /* Refuse a pending message before changing any handshake state. */
 if(io->read(io->context,*box))return -2;
 io->write(io->context,0x10,io->read(io->context,0x0c));
 io->write(io->context,0x24,0xffff);io->ring(io->context);
 if(op13_wifi_qmp_wait(io,0x28,0xffff))return -3;
 io->write(io->context,0x2c,0xffff);io->ring(io->context);
 if(op13_wifi_qmp_wait(io,0x14,0xffff))return -4;
 io->write(io->context,0x18,0xffff);io->ring(io->context);
 return op13_wifi_qmp_wait(io,0x30,0xffff)?-5:0;
}
static int op13_wifi_qmp_send(const struct op13_wifi_qmp_io *io,uint32_t box,const char *text)
{
 uint32_t expected=0,words[16]={0};
 if(op13_wifi_qmp_box(io,&expected)||box!=expected||io->read(io->context,box))return -1;
 unsigned len=0;while(len<64&&text[len])len++;
 if(len==64)return -2;
 for(unsigned i=0;i<len;i++)words[i/4]|=(uint32_t)(uint8_t)text[i]<<((i%4)*8);
 for(unsigned i=0;i<16;i++)io->write(io->context,box+4+i*4,words[i]);
 io->write(io->context,box,64);io->ring(io->context);
 return op13_wifi_qmp_wait(io,box,0)?-3:0;
}
#endif
