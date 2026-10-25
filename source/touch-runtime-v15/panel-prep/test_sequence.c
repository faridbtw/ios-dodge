#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "sm8750_panel.h"
static uint32_t regs[4];
static unsigned call,fail_at,sticky,fail_supply,fail_restore,avdd_calls,vdd_calls,fail_destroy;
static unsigned elapsed;
static int ix(uint32_t o){assert(o==0xa1000||o==0xa1004||o==0xa2000||o==0xa2004);return o==0xa1000?0:o==0xa1004?1:o==0xa2000?2:3;}
static int fault(void){call++;return call==fail_at||(sticky&&call>=fail_at);}
static int rd(void*c,uint32_t o,uint32_t*v){(void)c;if(fault())return -1;*v=regs[ix(o)];return 0;}
static int wr(void*c,uint32_t o,uint32_t v){(void)c;assert(o!=0xa2004);/* Model bus error AFTER write landed. */regs[ix(o)]=v;return fault()?-1:0;}
static void low(void){assert((regs[0]&0x3ff)==0x2c0&&!(regs[1]&2));assert((regs[2]&0x3ff)==0xc0);}
int sm8750_loader_avdd_enable(efi_boot_services*b,int e,struct sm8750_avdd_session*s){(void)b;assert(e&&!vdd_calls);low();avdd_calls++;s->stage=2;s->owned=1;return fail_supply==1?-1:0;}
int sm8750_loader_power_vote(efi_boot_services*b,int e,struct sm8750_power_session*s){(void)b;assert(e&&avdd_calls==1);low();vdd_calls++;s->npa_clients[0]=(void*)1;return fail_supply==2?-1:0;}
int sm8750_loader_avdd_restore(efi_boot_services*b,struct sm8750_avdd_session*s){(void)b;if(!s->owned)return 0;low();if(fail_restore)return -1;s->owned=0;return 0;}
void sm8750_loader_power_release(efi_boot_services*b,struct sm8750_power_session*s){(void)b;if(!s->npa_clients[0])return;low();assert(!fail_restore);if(!fail_destroy)s->npa_clients[0]=0;}
static efi_status stall(uint64_t us){assert(avdd_calls);if(us==200000||us==10000)low();if(us==80000)assert(regs[1]&2);elapsed+=(unsigned)us;return fault()?((uint64_t)1<<63):0;}
void op13_trace(uint32_t s,uint32_t d){(void)s;(void)d;}
static void reset(void){regs[0]=1;regs[1]=0;regs[2]=1;regs[3]=0;call=fail_at=sticky=fail_supply=fail_restore=avdd_calls=vdd_calls=elapsed=fail_destroy=0;}
static void restored(struct sm8750_panel*s){assert(!s->pins_owned&&!s->avdd.owned&&!s->power.npa_clients[0]);assert(regs[0]==1&&regs[1]==0&&regs[2]==1&&regs[3]==0);}
int main(void){efi_boot_services bs={.stall=stall};struct sm8750_panel_io io={0,rd,wr};struct sm8750_panel s={0};
 reset();assert(!sm8750_panel_prepare(&s,&bs,&io,1));unsigned max=call;assert(elapsed==290000&&s.prepared);assert(!sm8750_panel_release(&s));restored(&s);
 /* Every read/write/delay failure in the actual prepare function, including
  * failures after writes land; never replace production sequencing in test. */
 for(unsigned n=1;n<=max;n++){reset();memset(&s,0,sizeof(s));fail_at=n;assert(sm8750_panel_prepare(&s,&bs,&io,1)!=0);fail_at=0;assert(!sm8750_panel_release(&s));if(n>4)restored(&s);else assert(!avdd_calls&&!vdd_calls);}
 for(unsigned n=1;n<=2;n++){reset();memset(&s,0,sizeof(s));fail_supply=n;assert(sm8750_panel_prepare(&s,&bs,&io,1)==-1);restored(&s);assert(avdd_calls==1&&vdd_calls==(n==2));}
 reset();memset(&s,0,sizeof(s));fail_at=6;sticky=1;assert(sm8750_panel_prepare(&s,&bs,&io,1)==-1&&s.pins_owned);assert(!avdd_calls&&!vdd_calls);fail_at=sticky=0;assert(!sm8750_panel_release(&s));restored(&s);
 reset();memset(&s,0,sizeof(s));fail_supply=2;fail_restore=1;assert(sm8750_panel_prepare(&s,&bs,&io,1)==-1&&s.avdd.owned&&s.power.npa_clients[0]);assert(sm8750_panel_prepare(&s,&bs,&io,1)==-6);fail_restore=0;assert(!sm8750_panel_release(&s));restored(&s);
 /* Release has nine pin operations. Any failure must retain snapshots and
  * supply ownership, including failures after a restore write landed. */
 for(unsigned n=1;n<=16;n++){
  reset();memset(&s,0,sizeof(s));assert(!sm8750_panel_prepare(&s,&bs,&io,1));
  fail_at=call+n;assert(sm8750_panel_release(&s)==-1);
  assert(s.pins_owned);if(n<=6)assert(s.avdd.owned&&s.power.npa_clients[0]);
  fail_at=0;assert(!sm8750_panel_release(&s));restored(&s);
 }
 reset();memset(&s,0,sizeof(s));assert(!sm8750_panel_prepare(&s,&bs,&io,1));fail_destroy=1;
 assert(sm8750_panel_release(&s)==-1&&s.pins_owned&&s.power.npa_clients[0]&&s.power_settle_pending);low();assert(elapsed==290000);
 fail_destroy=0;assert(!sm8750_panel_release(&s));restored(&s);assert(elapsed==490000&&!s.power_settle_pending);
 reset();memset(&s,0,sizeof(s));assert(!sm8750_panel_prepare(&s,&bs,&io,1));fail_at=call+7;
 assert(sm8750_panel_release(&s)==-1&&s.power_settle_pending&&s.pins_owned&&!s.avdd.owned&&!s.power.npa_clients[0]);low();
 assert(sm8750_panel_prepare(&s,&bs,&io,1)==-6);fail_at=0;assert(!sm8750_panel_release(&s));restored(&s);assert(!s.power_settle_pending&&elapsed==690000);
 puts("PASS actual panel.c: reset-low before AVDD/VDD/delays; every prepare IO/delay fault, post-write failures, partial supplies, retained ownership and retry");
}
