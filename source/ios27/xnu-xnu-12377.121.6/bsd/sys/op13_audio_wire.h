/* SPDX-License-Identifier: BSD-2-Clause
 * Bounded in-band AudioReach APM wire messages. Layouts verified against
 * Linux sound/soc/qcom/qdsp6/audioreach.[ch] and AudioReach engine APIs.
 * IDs must be >=0x4001; Generic container type0x0b001001; RX/perf low latency2.
 * No DMA or amplifier commands.
 */
#ifndef OP13_AUDIO_WIRE_H
#define OP13_AUDIO_WIRE_H
#include <stdint.h>
#define OP13_AR_OPEN 0x01001000u
#define OP13_AR_CLOSE 0x01001004u
#define OP13_AR_SG 0x4001u
#define OP13_AR_CONTAINER 0x5001u
struct op13_ar_words {uint32_t data[128];unsigned count;int failed;};
static void op13_ar_word(struct op13_ar_words *b,uint32_t value)
{if(b->count>=128){b->failed=1;return;}b->data[b->count++]=value;}
static void op13_ar_module_param(struct op13_ar_words *b,uint32_t instance,uint32_t id,const uint32_t *words,unsigned count)
{
 unsigned aligned=(count+1)&~1u;
 if(count>96||b->count+4+aligned>128){b->failed=1;return;}
 op13_ar_word(b,instance);op13_ar_word(b,id);op13_ar_word(b,aligned*4);op13_ar_word(b,0);
 for(unsigned i=0;i<count;i++)op13_ar_word(b,words[i]);
 if(count!=aligned)op13_ar_word(b,0);
}
static void op13_ar_param(struct op13_ar_words *b,uint32_t id,const uint32_t *words,unsigned count)
{op13_ar_module_param(b,1,id,words,count);}
static unsigned op13_ar_graph_open(struct op13_ar_words *b)
{
 b->count=0;b->failed=0;
 const uint32_t sg[]={1,OP13_AR_SG,3,0x0800100e,4,2,0x0800100f,4,2,0x08001010,4,1};
 const uint32_t cont[]={1,OP13_AR_CONTAINER,4,0x08001011,8,1,0x0b001001,0x08001012,4,1,
  0x08001013,4,16384,0x08001014,4,2};
 const uint32_t modules[]={1,OP13_AR_SG,OP13_AR_CONTAINER,3,
  0x07001000,0x6000,0x07001005,0x6001,0x0700100a,0x6002};
 const uint32_t props[]={3,
  0x6000,1,0x08001015,8,0,1,
  0x6001,1,0x08001015,8,1,1,
  0x6002,1,0x08001015,8,1,0};
 const uint32_t conn[]={2,0x6000,1,0x6001,2,0x6001,1,0x6002,2};
 op13_ar_param(b,0x08001001,sg,sizeof(sg)/4);
 op13_ar_param(b,0x08001000,cont,sizeof(cont)/4);
 op13_ar_param(b,0x08001002,modules,sizeof(modules)/4);
 op13_ar_param(b,0x08001003,props,sizeof(props)/4);
 op13_ar_param(b,0x08001004,conn,sizeof(conn)/4);
 return b->failed?0:b->count*4;
}
static unsigned op13_ar_graph_close(struct op13_ar_words *b)
{
 b->count=0;b->failed=0;
 const uint32_t list[]={1,OP13_AR_SG};
 op13_ar_param(b,0x08001005,list,2);
 return b->failed?0:b->count*4;
}
static unsigned op13_ar_media(struct op13_ar_words *b)
{
 b->count=0;b->failed=0;
 const uint32_t pcm[]={1,0x09001000,20,48000,16|(1u<<16),16|(15u<<16),1|(2u<<16),0x0201};
 op13_ar_module_param(b,0x6000,0x0800100c,pcm,8);
 return b->failed?0:b->count*4;
}
static unsigned op13_ar_sink_config_line(struct op13_ar_words *b,unsigned line)
{
 b->count=0;b->failed=0;
 const uint32_t decoder[]={1,0x09001000,20,24|(1u<<16),24|(23u<<16),1|(1u<<16),2u<<16,0x0201};
 if(line!=1&&line!=2){b->failed=1;return 0;}
 const uint32_t i2s[]={0,1,line|(1u<<16)}; /* LPAIF,secondary,SD0=1/SD1=2,internal WS */
 const uint32_t mf[]={48000,24|(2u<<16),1};
 const uint32_t factor[]={1};
 op13_ar_module_param(b,0x6001,0x08001008,decoder,8);
 op13_ar_module_param(b,0x6002,0x08001019,i2s,3);
 op13_ar_module_param(b,0x6002,0x08001017,mf,3);
 op13_ar_module_param(b,0x6002,0x08001018,factor,1);
 return b->failed?0:b->count*4;
}
static unsigned op13_ar_sink_config(struct op13_ar_words *b)
{return op13_ar_sink_config_line(b,1);}
static unsigned op13_ar_prm_core(struct op13_ar_words *b)
{
 b->count=0;b->failed=0;
 const uint32_t p[]={2,0x08001032,4,0,1}; /* PRM,LPASS core */
 for(unsigned i=0;i<5;i++)op13_ar_word(b,p[i]);
 return b->count*4;
}
static unsigned op13_ar_prm_clock(struct op13_ar_words *b,int on)
{
 b->count=0;b->failed=0;
 const uint32_t p[]={2,0x0800102c,on?20u:8u,0,1,0x102,3072000,1,0};
 for(unsigned i=0;i<(on?9u:6u);i++)op13_ar_word(b,p[i]);
 return b->count*4;
}
#endif
