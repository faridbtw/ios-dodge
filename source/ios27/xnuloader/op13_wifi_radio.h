/* SPDX-License-Identifier: BSD-2-Clause
 * Native Kiwi/Peach target CE and service pipe wire configuration. Independent
 * encoding from pinned CNSS schemas and ISC CE assignment/HTC headers.
 */
#ifndef OP13_WIFI_RADIO_H
#define OP13_WIFI_RADIO_H
#include "op13_wifi_qmi_init.h"
static size_t op13_qmi_wlan_config(uint8_t *b,size_t capacity)
{
 /* All nine native target pipes. Host-only CE7 has direction4/zero size;
  * CE6/8 are firmware-owned; CE9..11 belong to MHI and are never configured. */
 static const uint32_t pipes[9][5]={
  {0,2,32,2048,0},{1,1,32,2048,0},{2,1,32,3520,0},{3,2,32,3520,0},
  {4,2,256,256,8},{5,1,0,2048,0},{6,3,32,16384,0},{7,4,0,0,8},{8,3,32,16384,0},
 };
 /* Native non-direct-link service map: control/HTC is returned on CE2. */
 static const uint32_t services[14][3]={
  {0x104,2,3},{0x104,1,2},{0x102,2,3},{0x102,1,2},
  {0x101,2,3},{0x101,1,2},{0x103,2,3},{0x103,1,2},
  {0x100,2,3},{0x100,1,2},{1,2,0},{1,1,2},{0x300,2,4},{0x300,1,1},
 };
 size_t total=4+9*20+4+14*12+4;if(!b||capacity<total)return 0;
 size_t at=0;b[at++]=0x11;b[at++]=(1+9*20)&255;b[at++]=(1+9*20)>>8;b[at++]=9;
 for(unsigned i=0;i<9;i++)for(unsigned j=0;j<5;j++){op13_qrtr_put(b+at,pipes[i][j]);at+=4;}
 b[at++]=0x12;b[at++]=(1+14*12)&255;b[at++]=(1+14*12)>>8;b[at++]=14;
 for(unsigned i=0;i<14;i++)for(unsigned j=0;j<3;j++){op13_qrtr_put(b+at,services[i][j]);at+=4;}
 /* Native PEACH uses shadow-v3; no shadow writes requested in direct-MMIO polling mode. */
 b[at++]=0x17;b[at++]=1;b[at++]=0;b[at++]=0;
 return at;
}
struct op13_htc_frame {unsigned endpoint,flags,payload_bytes,data_bytes,message,credits,credit_size,max_endpoints,service,status,assigned,max_message;};
static int op13_htc_decode(const uint8_t *b,size_t bytes,struct op13_htc_frame *out)
{
 if(!b||!out||bytes<8)return -1;
 struct op13_htc_frame p={0};p.endpoint=b[0];p.flags=b[1];p.payload_bytes=op13_qmi_u16(b+2);
 if(p.endpoint>=32||p.payload_bytes>bytes-8)return -2;
 p.data_bytes=p.payload_bytes;
 if(p.flags&2){if(b[4]>p.payload_bytes)return -9;p.data_bytes-=b[4];}
 if(p.endpoint==0&&p.data_bytes){
  if(p.data_bytes<2)return -3;p.message=op13_qmi_u16(b+8);
  if(p.message==1){
   if(p.data_bytes<8)return -4;
   p.credits=op13_qmi_u16(b+10);p.credit_size=op13_qmi_u16(b+12);p.max_endpoints=b[14];
   if(!p.credits||!p.credit_size||p.credit_size>4096||!p.max_endpoints||p.max_endpoints>32)return -5;
  }
  if(p.message==3){
   if(p.data_bytes<12)return -6;
   p.service=op13_qmi_u16(b+10);p.status=b[12];p.assigned=b[13];p.max_message=op13_qmi_u16(b+14);
   if(b[16]>p.data_bytes-12)return -7;
   if(!p.status&&(!p.assigned||p.assigned>=32||p.max_message<4))return -8;
  }
 }
 *out=p;return 0;
}
static size_t op13_htc_encode(uint8_t *b,size_t capacity,unsigned endpoint,unsigned seq,
                              const uint8_t *payload,size_t bytes)
{
 if(!b||!payload||endpoint>=32||!bytes||bytes>4088||capacity<bytes+8)return 0;
 b[0]=endpoint;b[1]=0;b[2]=bytes;b[3]=bytes>>8;b[4]=0;b[5]=seq;b[6]=b[7]=0;
 for(size_t i=0;i<bytes;i++)b[8+i]=payload[i];return 8+bytes;
}
static int op13_ce_tx_desc(uint32_t b[4],uint64_t address,unsigned bytes,uint64_t first,uint64_t last)
{
 if(!b||!bytes||bytes>3520||address<first||address>=last||bytes>last-address||(address&3)||address>>40)return -1;
 b[0]=(uint32_t)address;b[1]=(uint32_t)(address>>32)|(bytes<<16);b[2]=0;b[3]=0;return 0;
}
#endif
