/* SPDX-License-Identifier: BSD-2-Clause
 * Bounded QRTR wire decoding for the RAM-only IPCR diagnostic.
 * Protocol reference: Linux net/qrtr and UniXNU Ath12kWifi nw_qrtr.c.
 * No socket stack, credentials, scan or association implemented here.
 */
#ifndef OP13_WIFI_QRTR_H
#define OP13_WIFI_QRTR_H
#include <stdint.h>
#include <stddef.h>
struct op13_qrtr_packet {
 uint32_t type,src_node,src_port,dst_node,dst_port,confirm,size;
 const uint8_t *payload;
};
static uint32_t op13_qrtr_u32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint32_t op13_qrtr_u16(const uint8_t *p)
{uint32_t v=(uint32_t)p[0]|(uint32_t)p[1]<<8;return v==0xffff?~0u:v;}
static void op13_qrtr_put(uint8_t *p,uint32_t v)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
static int op13_qrtr_decode(const void *data,size_t n,struct op13_qrtr_packet *p)
{
 const uint8_t *b=data;size_t header;
 if(!b||!p||n<4)return -1;
 if(op13_qrtr_u32(b)==1&&n>=32){
  header=32;p->type=op13_qrtr_u32(b+4);p->src_node=op13_qrtr_u32(b+8);
  p->src_port=op13_qrtr_u32(b+12);p->confirm=op13_qrtr_u32(b+16);
  p->size=op13_qrtr_u32(b+20);p->dst_node=op13_qrtr_u32(b+24);p->dst_port=op13_qrtr_u32(b+28);
 }else if(b[0]==3&&n>=16){
  header=16+(size_t)b[3];p->type=b[1];p->confirm=b[2]&1;p->size=op13_qrtr_u32(b+4);
  p->src_node=op13_qrtr_u16(b+8);p->src_port=op13_qrtr_u16(b+10);if(p->src_port==~0u)p->src_port=0xfffffffeu;
  p->dst_node=op13_qrtr_u16(b+12);p->dst_port=op13_qrtr_u16(b+14);if(p->dst_port==~0u)p->dst_port=0xfffffffeu;
 }else return -1;
 if(header>n||p->size>n-header)return -1;
 p->payload=b+header;return 0;
}
static void op13_qrtr_hello(uint8_t packet[52])
{
 for(unsigned i=0;i<52;i++)packet[i]=0;
 op13_qrtr_put(packet,1);op13_qrtr_put(packet+4,2);op13_qrtr_put(packet+8,1);
 op13_qrtr_put(packet+12,0xfffffffeu);op13_qrtr_put(packet+20,20);
 op13_qrtr_put(packet+24,~0u);op13_qrtr_put(packet+28,0xfffffffeu);op13_qrtr_put(packet+32,2);
}
#endif
