/* SPDX-License-Identifier: BSD-2-Clause
 * Native CNSS notification/host-capability wire formats. Diagnostic only:
 * collect memory requirements; do not supply unowned addresses to firmware.
 */
#ifndef OP13_WIFI_QMI_INIT_H
#define OP13_WIFI_QMI_INIT_H
#include "op13_wifi_qmi.h"
struct op13_qmi_message {uint32_t flags,transaction,id;size_t size;const uint8_t *tlvs;};
struct op13_qmi_memory {
 unsigned count;
 struct {uint32_t size,type,cfg_count;struct {uint64_t offset;uint32_t size;unsigned secure;} cfg[2];} seg[52];
};
static int op13_qmi_message_decode(const uint8_t *b,size_t n,struct op13_qmi_message *p)
{
 if(n<7||op13_qmi_u16(b+5)!=n-7)return -1;
 p->flags=b[0];p->transaction=op13_qmi_u16(b+1);p->id=op13_qmi_u16(b+3);p->size=n-7;p->tlvs=b+7;
 for(size_t at=0;at<p->size;){
  if(p->size-at<3)return -1;
  size_t len=op13_qmi_u16(p->tlvs+at+1);at+=3;if(len>p->size-at)return -1;at+=len;
 }
 return 0;
}
static int op13_qmi_result(const struct op13_qmi_message *p,uint32_t *result,uint32_t *error)
{
 if(p->flags!=2)return -1;
 unsigned found=0;
 for(size_t at=0;at<p->size;){
  unsigned type=p->tlvs[at];size_t len=op13_qmi_u16(p->tlvs+at+1);at+=3;
  if(type==2){if(len!=4||found++)return -1;*result=op13_qmi_u16(p->tlvs+at);*error=op13_qmi_u16(p->tlvs+at+2);}
  at+=len;
 }
 return found?0:-1;
}
static int op13_qmi_mem_decode(const struct op13_qmi_message *p,struct op13_qmi_memory *m)
{
 if(p->flags!=4||p->id!=0x35)return -1;
 m->count=0;unsigned found=0;
 for(size_t at=0;at<p->size;){
  unsigned type=p->tlvs[at];size_t len=op13_qmi_u16(p->tlvs+at+1);at+=3;
  const uint8_t *v=p->tlvs+at;at+=len;
  if(type!=1)continue;
  if(!len||found++||v[0]>52)return -1;
  unsigned count=v[0];size_t pos=1;
  for(unsigned i=0;i<count;i++){
   if(len-pos<9)return -1;
   m->seg[i].size=op13_qrtr_u32(v+pos);m->seg[i].type=op13_qrtr_u32(v+pos+4);
   unsigned n=v[pos+8];pos+=9;if(n>2||n*13>len-pos)return -1;m->seg[i].cfg_count=n;
   for(unsigned j=0;j<n;j++,pos+=13){
    uint64_t off=op13_qrtr_u32(v+pos)|(uint64_t)op13_qrtr_u32(v+pos+4)<<32;
    uint32_t size=op13_qrtr_u32(v+pos+8);unsigned secure=v[pos+12];
    if(off>m->seg[i].size||size>m->seg[i].size-off||secure>1)return -1;
    m->seg[i].cfg[j].offset=off;m->seg[i].cfg[j].size=size;m->seg[i].cfg[j].secure=secure;
   }
  }
  if(pos!=len)return -1;m->count=count;
 }
 return found?0:-1;
}
static size_t op13_qmi_request(uint8_t *b,size_t capacity,uint32_t node,uint32_t port,unsigned transaction,unsigned id,const uint8_t *tlvs,size_t size)
{
 size_t length=39+size,padded=(length+3)&~(size_t)3;
 if(size>6200||padded>capacity||transaction>65535||id>65535)return 0;
 for(size_t i=0;i<padded;i++)b[i]=0;
 op13_qrtr_put(b,1);op13_qrtr_put(b+4,1);op13_qrtr_put(b+8,1);op13_qrtr_put(b+12,0x4000);
 op13_qrtr_put(b+20,7+size);op13_qrtr_put(b+24,node);op13_qrtr_put(b+28,port);
 b[33]=transaction;b[34]=transaction>>8;b[35]=id;b[36]=id>>8;b[37]=size;b[38]=size>>8;
 for(size_t i=0;i<size;i++)b[39+i]=tlvs[i];return padded;
}
/* First32MiB contains live host/TX/RDDM buffers. Last page holds RX vectors.
 * Plan the whole request before giving the endpoint any new addresses. */
static int op13_qmi_memory_plan(const struct op13_qmi_memory *m,uint64_t pool_bytes,uint64_t offsets[52])
{
 uint64_t at=0x2000000ULL;unsigned seen=0;
 if(!m->count||m->count>3||pool_bytes<at+4096)return -1;
 for(unsigned i=0;i<m->count;i++){
  uint32_t type=m->seg[i].type;uint64_t size=m->seg[i].size;
  if((type!=1&&type!=4&&type!=11)||!size||size>pool_bytes||m->seg[i].cfg_count)return -1;
  if(seen&(1u<<type))return -1;seen|=1u<<type;
  uint64_t align=4096;while(align<size)align<<=1;
  at=(at+align-1)&~(align-1);
  if(at>pool_bytes-4096||size>pool_bytes-4096-at)return -1;
  offsets[i]=at;at+=size;
 }
 return 0;
}
/* Native BDF request: total_size is remaining bytes, not original length. */
static size_t op13_qmi_bdf_chunk(uint8_t *b,size_t capacity,unsigned board,unsigned type,unsigned segment,
                               uint32_t remaining,const uint8_t *data,size_t bytes)
{
 if(!bytes||bytes>6144||bytes>remaining||capacity<38+bytes||!data||type>255)return 0;
 size_t at=0;
 b[at++]=1;b[at++]=1;b[at++]=0;b[at++]=1;
 const uint32_t values[]={board,remaining,segment};
 for(unsigned i=0;i<3;i++){b[at++]=0x10+i;b[at++]=4;b[at++]=0;op13_qrtr_put(b+at,values[i]);at+=4;}
 b[at++]=0x13;b[at++]=(bytes+2)&255;b[at++]=(bytes+2)>>8;b[at++]=bytes&255;b[at++]=bytes>>8;
 for(size_t i=0;i<bytes;i++)b[at++]=data[i];
 b[at++]=0x14;b[at++]=1;b[at++]=0;b[at++]=bytes==remaining;
 b[at++]=0x15;b[at++]=1;b[at++]=0;b[at++]=type;
 return at;
}
/* Native QDSS config: TLVs10 total remaining,11 segment,12 bytes,13 end. */
static size_t op13_qmi_config_chunk(uint8_t *b,size_t capacity,unsigned segment,
                                  uint32_t remaining,const uint8_t *data,size_t bytes)
{
 if(!b||!data||!bytes||bytes>6144||bytes>remaining||capacity<23+bytes)return 0;
 size_t at=0;const uint32_t values[]={remaining,segment};
 for(unsigned i=0;i<2;i++){b[at++]=0x10+i;b[at++]=4;b[at++]=0;op13_qrtr_put(b+at,values[i]);at+=4;}
 b[at++]=0x12;b[at++]=(bytes+2)&255;b[at++]=(bytes+2)>>8;b[at++]=bytes&255;b[at++]=bytes>>8;
 for(size_t i=0;i<bytes;i++)b[at++]=data[i];
 b[at++]=0x13;b[at++]=1;b[at++]=0;b[at++]=bytes==remaining;
 return at;
}
#endif
