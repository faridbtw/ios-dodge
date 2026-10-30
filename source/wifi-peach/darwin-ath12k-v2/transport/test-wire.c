#include <assert.h>
#include <stdio.h>
#include "op13_peach_wire.h"
int main(void)
{
 uint8_t packet[96],data[65]={0,1,0x5a,0x24};struct op13_qrtr_packet decoded;
 size_t n=op13_peach_qrtr_encode(packet,sizeof packet,1,0x4000,7,1,data,7);
 assert(n==40&&packet[39]==0&&!op13_qrtr_decode(packet,n,&decoded));
 assert(decoded.type==1&&decoded.size==7&&decoded.src_port==0x4000&&decoded.dst_node==7&&decoded.dst_port==1);
 assert(!memcmp(decoded.payload,data,7));
 for(size_t len=1;len<=64;len++){
  memset(packet,0xa5,sizeof packet);n=op13_peach_qrtr_encode(packet,sizeof packet,1,0x4000,7,1,data,len);
  assert(n==32+((len+3)&~(size_t)3));
  assert(!op13_qrtr_decode(packet,n,&decoded)&&decoded.size==len);
  for(size_t i=32+len;i<n;i++)assert(!packet[i]);
  for(size_t cap=0;cap<n;cap++)assert(!op13_peach_qrtr_encode(packet,cap,1,0x4000,7,1,data,len));
 }
 assert(!op13_peach_qrtr_encode(packet,sizeof packet,1,0x4000,7,1,data,65));
 assert(!op13_peach_qrtr_encode(packet,sizeof packet,1,0x4000,7,1,data,0));
 puts("QRTR payload length, MHI padding, zero fill, all64 lengths and short-capacity rejection passed");
}
