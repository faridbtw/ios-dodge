/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Mode-0 loop adapted from tycykp/UniXNU-OnePlus-Ace5 giulia_touch_gpio.h,
 * commit18f51ec652501c62e19dd4f1035dc51daa2a3361, and the HackiOS GPIO SPI
 * fallback. OnePlus13 wiring is independently pinned in touch/REFERENCES.md.
 * Fallible callbacks, snapshot ownership and restoration are local additions.
 */
#include "op13_bitbang.h"
static uint32_t off(unsigned p,unsigned r){return (48u+p)*0x1000u+r;}
static int rd(struct op13_bitbang *s,unsigned p,unsigned r,uint32_t *v)
{return s->io.read(s->io.cookie,off(p,r),v);}
static int wr(struct op13_bitbang *s,unsigned p,unsigned r,uint32_t v,uint32_t mask)
{
 uint32_t got;
 if(s->io.write(s->io.cookie,off(p,r),v)||rd(s,p,r,&got))return -1;
 return ((v^got)&mask)?-1:0;
}
static int level(struct op13_bitbang *s,unsigned p,unsigned high)
{
 uint32_t v;
 if(p==0||rd(s,p,4,&v))return -1;
 return wr(s,p,4,(v&~2u)|(high?2u:0),2);
}
static int config(struct op13_bitbang *s,unsigned p,unsigned output)
{
 static const uint32_t drive[4]={0x80,0x40,0x80,0x40};
 return wr(s,p,0,(s->saved_cfg[p]&~0x3ffu)|drive[p]|(output?0x200u:0),0x3ff);
}
int op13_bitbang_open(struct op13_bitbang *s,const struct op13_bitbang_io *io,int enabled)
{
 if(!s||!io)return -5;
 if(!enabled)return -3;
 if(s->owned)return -6;
 if(!io->read||!io->write||!io->delay)return -3;
 s->io=*io;s->ready=s->poisoned=0;
 for(unsigned i=0;i<4;i++){
  if(rd(s,i,0,&s->saved_cfg[i])||rd(s,i,4,&s->saved_io[i]))return -1;
  if(s->saved_cfg[i]&0x1800u)return -3;
 }
 /* A failed write may already have taken effect. Claim BEFORE first write. */
 s->owned=1;
 if(level(s,3,1)||config(s,3,1)||config(s,0,0)||
    level(s,1,1)||config(s,1,1)||level(s,2,0)||config(s,2,1))return -1;
 s->ready=1;return 0;
}
int op13_bitbang_transfer(struct op13_bitbang *s,const uint8_t *tx,uint8_t *rx,size_t n)
{
 if(!s||!s->ready||s->poisoned)return -6;
 if(!n||n>1032||(!tx&&!rx))return -5;
 if(level(s,2,0)||level(s,3,0)||s->io.delay(s->io.cookie,5))goto fail;
 for(size_t i=0;i<n;i++){
  uint8_t out=tx?tx[i]:255,in=0;
  for(unsigned bit=0;bit<8;bit++){
   uint32_t sample;
   if(level(s,1,(out>>(7-bit))&1)||s->io.delay(s->io.cookie,5)||
      level(s,2,1)||s->io.delay(s->io.cookie,5)||rd(s,0,4,&sample)||
      level(s,2,0))goto fail;
   in=(uint8_t)((in<<1)|(sample&1));
  }
  if(rx)rx[i]=in;
 }
 if(s->io.delay(s->io.cookie,5)||level(s,3,1)||s->io.delay(s->io.cookie,5))goto fail;
 return 0;
fail:
 s->ready=0;s->poisoned=1;
 (void)level(s,3,1);
 return -1;
}
int op13_bitbang_cleanup(struct op13_bitbang *s)
{
 if(!s)return -5;
 if(!s->owned)return 0;
 s->ready=0;
 /* Reset is held low by caller. Deassert CS before making all pins high-Z.
  * Restore latches only while input, then restore the original configurations.
  * Do not touch MISO's latch: it was never written by this transport. */
 if(level(s,3,1))return -1;
 for(unsigned i=0;i<4;i++){
  uint32_t v;
  if(rd(s,i,0,&v)||wr(s,i,0,(v&~0x3fcu),0x3fc))return -1;
 }
 for(unsigned i=1;i<4;i++){
  uint32_t v;
  if(rd(s,i,4,&v)||wr(s,i,4,(v&~2u)|(s->saved_io[i]&2u),2))return -1;
 }
 for(unsigned i=0;i<4;i++)if(wr(s,i,0,s->saved_cfg[i],0x3ff))return -1;
 s->owned=0;return 0;
}
