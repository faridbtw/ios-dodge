/* Actual transport tests: mode-0 edges, FF reads, CS intervals, landed faults. */
#include "op13_bitbang.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct fixture {
 uint32_t cfg[4], io[4]; unsigned calls, fail, writes, edges, selects, deselects;
 unsigned bits; uint8_t sent[1032], incoming[1032];
};
static int trip(struct fixture *f) {return ++f->calls==f->fail;}
static int read32(void *c,uint32_t off,uint32_t *v)
{
 struct fixture *f=c;unsigned p=off/4096-48,r=off%4096;
 assert(p<4&&(r==0||r==4));if(trip(f))return -1;
 *v=r?f->io[p]:f->cfg[p];return 0;
}
static int write32(void *c,uint32_t off,uint32_t v)
{
 struct fixture *f=c;unsigned p=off/4096-48,r=off%4096;
 assert(p<4&&(r==0||r==4));assert(!(p==0&&r==4));f->writes++;
 if(!r)f->cfg[p]=v;
 else {
  unsigned old=f->io[p];f->io[p]=v;
  if(p==3&&(old^v)&2){if(v&2)f->deselects++;else f->selects++;}
  if(p==2&&!(old&2)&&(v&2)){
   assert(!(f->io[3]&2));unsigned i=f->bits++;
   assert(i<sizeof(f->sent)*8);
   f->sent[i/8]=(uint8_t)((f->sent[i/8]<<1)|((f->io[1]>>1)&1));
   f->io[0]=(f->io[0]&~1u)|((f->incoming[i/8]>>(7-i%8))&1);
   f->edges++;
  }
 }
 return trip(f)?-1:0; /* write may land before reporting failure */
}
static int pause_us(void *c,unsigned us){assert(us==5);return trip(c)?-1:0;}
static void init(struct fixture *f)
{
 memset(f,0,sizeof(*f));
 for(unsigned i=0;i<4;i++){f->cfg[i]=0x10000004+i*0x40;f->io[i]=i&1?2:0;}
 f->incoming[0]=0xa5;f->incoming[1]=0x3c;
}
static void restored(struct fixture *f,const struct fixture *orig)
{
 for(unsigned i=0;i<4;i++)assert(f->cfg[i]==orig->cfg[i]);
 for(unsigned i=1;i<4;i++)assert((f->io[i]&2)==(orig->io[i]&2));
}
int main(void)
{
 struct fixture f,orig;struct op13_bitbang b={0};uint8_t rx[2],tx[2]={0x96,0x69};
 init(&f);orig=f;struct op13_bitbang_io io={&f,read32,write32,pause_us};
 assert(op13_bitbang_open(&b,&io,0)==-3&&f.calls==0);
 assert(!op13_bitbang_open(&b,&io,1));unsigned open_calls=f.calls;
 f.bits=f.edges=f.selects=f.deselects=0;
 assert(!op13_bitbang_transfer(&b,tx,rx,2));
 assert(!memcmp(tx,f.sent,2)&&rx[0]==0xa5&&rx[1]==0x3c);
 assert(f.edges==16&&f.selects==1&&f.deselects==1&&!(f.io[2]&2));
 f.bits=f.edges=f.selects=f.deselects=0;memset(f.sent,0,sizeof(f.sent));
 unsigned start=f.calls;assert(!op13_bitbang_transfer(&b,0,rx,2));
 unsigned transfer_calls=f.calls-start;
 assert(f.sent[0]==255&&f.sent[1]==255&&rx[0]==0xa5&&rx[1]==0x3c);
 assert(!op13_bitbang_cleanup(&b));restored(&f,&orig);
 for(unsigned n=1;n<=open_calls;n++){
  init(&f);orig=f;memset(&b,0,sizeof(b));f.fail=n;
  assert(op13_bitbang_open(&b,&io,1));f.fail=0;
  assert(!op13_bitbang_cleanup(&b));restored(&f,&orig);
 }
 for(unsigned n=1;n<=transfer_calls;n++){
  init(&f);orig=f;memset(&b,0,sizeof(b));assert(!op13_bitbang_open(&b,&io,1));
  f.fail=f.calls+n;assert(op13_bitbang_transfer(&b,tx,rx,2));
  assert(b.poisoned&&!b.ready);f.fail=0;
  assert(!op13_bitbang_cleanup(&b));restored(&f,&orig);
 }
 init(&f);memset(&b,0,sizeof(b));f.cfg[2]|=0x800;
 assert(op13_bitbang_open(&b,&io,1)==-3&&!f.writes&&!b.owned);
 printf("bitbang: edges/FF/CS and %u open + %u transfer faults passed\n",open_calls,transfer_calls);
 return 0;
}
