/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef OP13_WIFI_WMI_COMMAND_H
#define OP13_WIFI_WMI_COMMAND_H
#include "op13_wifi_radio.h"
#include "op13_wifi_wmi_schema.h"
static int op13_htc_credit_delta(const uint8_t *p,size_t bytes,unsigned endpoint,unsigned *out)
{
 if(!p||!out||endpoint>=32)return -1;
 size_t at=0;unsigned total=0;
 while(at<bytes){
  if(bytes-at<4)return -2;
  unsigned id=p[at],n=p[at+1];at+=4;if(n>bytes-at)return -3;
  if(id==1){
   if(n%4)return -4;
   for(unsigned i=0;i<n;i+=4){if(p[at+i]>=32)return -5;if(p[at+i]==endpoint)total+=p[at+i+1];}
  }
  at+=n;
 }
 if(total>255)return -6;*out=total;return 0;
}
static int op13_wmi_validate(const uint8_t *b,size_t bytes,const uint32_t abi[6],int init_sent,int ready,unsigned *command)
{
 if(!b||!abi||!command||bytes<8||bytes>3512||(bytes&3)||b[3])return -1;
 unsigned cmd=op13_qrtr_u32(b);if(cmd!=1&&cmd!=0x5001&&cmd!=0x3001&&cmd!=0x3002&&cmd!=0x3003&&cmd!=0x4003)return -2;
 if(cmd!=1&&!ready)return -3;
 if(cmd==1&&init_sent)return -4;
 /* Native ath12k radio start, matched to stock Peach wmi_unified.h. Limit
  * this experiment to the exact pdev/parameter/value pairs being tested. */
 if(cmd==0x4003){
  if(bytes!=20||op13_qrtr_u32(b+4)!=((82u<<16)|12)||op13_qrtr_u32(b+8)!=1)return -13;
  unsigned id=op13_qrtr_u32(b+12),v=op13_qrtr_u32(b+16);
  if(!((id==0x22&&v==1)||(id==0xa&&v==1)||(id==0x23&&v==0)||((id==1||id==2)&&v==3)))return -14;
 }

 size_t at=4;unsigned count=0;
 const unsigned tags[6]={74,75,18,515,18,18};
 const unsigned lengths[6]={28,OP13_WMI_RESOURCE_BYTES-4,0,12,0,0};
 while(at<bytes){
  if(bytes-at<4)return -5;
  uint32_t h=op13_qrtr_u32(b+at);unsigned tag=h>>16,n=h&65535;
  if(n>bytes-at-4||(n&3))return -6;
  if(cmd==1){
   if(count>=6||tag!=tags[count]||n!=lengths[count])return -7;
   if(count==0){
    for(unsigned i=0;i<6;i++)if(op13_qrtr_u32(b+at+4+4*i)!=abi[i])return -8;
    if(op13_qrtr_u32(b+at+28))return -9; /* No host memory chunks requested. */
   }
   if(count==1){
    const uint8_t *r=b+at;
    unsigned vdevs=op13_qrtr_u32(r+OP13_RSC_NUM_VDEVS*4),peers=op13_qrtr_u32(r+OP13_RSC_NUM_PEERS*4);
    unsigned tids=op13_qrtr_u32(r+OP13_RSC_NUM_TIDS*4),descs=op13_qrtr_u32(r+OP13_RSC_NUM_MSDU_DESC*4);
    if(!vdevs||vdevs>16||peers<=vdevs||peers>128||tids<peers||tids>1024||!descs||descs>1024)return -10;
   }
   if(count==3){for(unsigned i=0;i<n;i++)if(b[at+4+i])return -11;}
  }
  count++;at+=4+n;
 }
 if(at!=bytes||!count||(cmd==1&&count!=6))return -12;
 *command=cmd;return 0;
}
#endif
