/* SPDX-License-Identifier: BSD-2-Clause
 * Read-only WLFW capability query and bounded QMI TLV decoding.
 * Message ID/layout checked against native CNSS wlan_firmware_service_v01.
 */
#ifndef OP13_WIFI_QMI_H
#define OP13_WIFI_QMI_H
#include "op13_wifi_qrtr.h"
struct op13_qmi_cap {
 uint32_t transaction,message,flags,result,error,chip,family,board,soc;
 unsigned has_result,has_chip,has_board,has_soc;
};
static uint32_t op13_qmi_u16(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8;}
static int op13_qmi_cap_decode(const uint8_t *b,size_t n,struct op13_qmi_cap *out)
{
 struct op13_qmi_cap p={0};
 if(n<7)return -1;
 p.flags=b[0];p.transaction=op13_qmi_u16(b+1);p.message=op13_qmi_u16(b+3);
 size_t length=op13_qmi_u16(b+5),at=7;
 if(length!=n-7||p.flags!=2||p.message!=0x24)return -1;
 while(at<n){
  if(n-at<3)return -1;
  unsigned type=b[at];size_t len=op13_qmi_u16(b+at+1);at+=3;
  if(len>n-at)return -1;
  if(type==2){if(len!=4||p.has_result)return -1;p.has_result=1;p.result=op13_qmi_u16(b+at);p.error=op13_qmi_u16(b+at+2);}
  if(type==0x10){if(len!=8||p.has_chip)return -1;p.has_chip=1;p.chip=op13_qrtr_u32(b+at);p.family=op13_qrtr_u32(b+at+4);}
  if(type==0x11){if(len!=4||p.has_board)return -1;p.has_board=1;p.board=op13_qrtr_u32(b+at);}
  if(type==0x12){if(len!=4||p.has_soc)return -1;p.has_soc=1;p.soc=op13_qrtr_u32(b+at);}
  at+=len;
 }
 if(!p.has_result)return -1;
 *out=p;return 0;
}
static void op13_qmi_cap_request(uint8_t b[40],uint32_t node,uint32_t port)
{
 for(unsigned i=0;i<40;i++)b[i]=0;
 op13_qrtr_put(b,1);op13_qrtr_put(b+4,1);op13_qrtr_put(b+8,1);op13_qrtr_put(b+12,0x4000);
 op13_qrtr_put(b+20,7);op13_qrtr_put(b+24,node);op13_qrtr_put(b+28,port);
 b[33]=1;b[35]=0x24;
}
#endif
