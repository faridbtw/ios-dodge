/* SPDX-License-Identifier: BSD-2-Clause
 * AudioReach shared-memory wire bodies, without the APM command envelope.
 * Layouts: upstream audioreach.h/q6apm.c. DSP address SID bits: actual
 * OnePlus msm_audio_ion.c and captured qcom,smmu-sid-mask=0xf, SID0x1001.
 */
#ifndef OP13_AUDIO_DATA_H
#define OP13_AUDIO_DATA_H
#include "op13_audio_wire.h"
#define OP13_AR_MAP 0x0100100cu
#define OP13_AR_MAP_REPLY 0x02001001u
#define OP13_AR_UNMAP 0x0100100du
#define OP13_AR_WRITE 0x0400100au
#define OP13_AR_WRITE_REPLY 0x05001004u
#define OP13_AR_DSP_ADDRESS 0x110000000ULL
static unsigned op13_ar_map(struct op13_ar_words *b)
{
 b->count=0;b->failed=0;
 const uint32_t w[]={3u|(1u<<16),0,0x10000000,1,32768};
 for(unsigned i=0;i<5;i++)op13_ar_word(b,w[i]);
 return b->count*4;
}
static unsigned op13_ar_write_period(struct op13_ar_words *b,uint32_t handle,unsigned period)
{
 b->count=0;b->failed=0;
 if(!handle||period>=8){b->failed=1;return 0;}
 const uint32_t w[]={0x10000000+period*4096,1,handle,3840,0,0,0xff00,0,0,0,0};
 for(unsigned i=0;i<11;i++)op13_ar_word(b,w[i]);return b->count*4;
}
static int op13_ar_period_reply(const uint32_t *w,unsigned bytes,uint32_t handle,unsigned period)
{
 if(!w||bytes!=32||!handle||period>=8||w[0]!=0x10000000+period*4096||w[1]!=1||w[2]!=handle)return -1;
 return w[3]||w[7]?-2:0;
}
static unsigned op13_ar_write_silence(struct op13_ar_words *b,uint32_t handle)
{
 b->count=0;b->failed=!handle;
 /* 100ms stereo16 PCM at48kHz, no timestamp, no metadata. */
 const uint32_t w[]={0x10000000,1,handle,19200,0,0,0,0,0,0,0};
 for(unsigned i=0;i<11;i++)op13_ar_word(b,w[i]);
 return b->failed?0:b->count*4;
}
/* GPR token/service matching is performed by the transport before this.
 * Reject truncated or unrelated DMA completions rather than accepting status0.
 */
static int op13_ar_data_reply(uint32_t opcode,const uint32_t *w,unsigned bytes,
 uint32_t handle,uint32_t *value)
{
 if(!w||!value)return -1;
 if(opcode==OP13_AR_MAP_REPLY){
  if(bytes!=4||!w[0])return -1;
  *value=w[0];return 0;
 }
 if(opcode==OP13_AR_WRITE_REPLY){
  if(bytes!=32||w[0]!=0x10000000||w[1]!=1||w[2]!=handle||!handle)return -1;
  *value=w[3]?w[3]:w[7];return 0;
 }
 return -1;
}
#endif
