#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sm8750_panel.h"
static uint32_t regs[4];
static unsigned writes,reads,fail_write,fail_after,delay,fail_power,fail_avdd,fail_release,drop_write;
static int idx(uint32_t o) {
 switch(o) {case 0xa1000:return 0;case 0xa1004:return 1;
 case 0xa2000:return 2;case 0xa2004:return 3;default:assert(0);return 0;}
}
static int rd(void *c,uint32_t o,uint32_t *v) {(void)c;reads++;*v=regs[idx(o)];return 0;}
static int wr(void *c,uint32_t o,uint32_t v) {
 (void)c;assert(o!=0xa2004);writes++;
 if(writes==fail_write||(fail_after&&writes>=fail_after)) return -1;
 if(writes==drop_write) return 0;
 regs[idx(o)]=v;return 0;
}
static efi_status stall(uint64_t us) {delay+=(unsigned)us;return 0;}
int sm8750_loader_power_vote(efi_boot_services *b,int en,struct sm8750_power_session *s)
{(void)b;assert(en);s->npa_clients[0]=(void *)1;return fail_power?-1:0;}
void sm8750_loader_power_release(efi_boot_services *b,struct sm8750_power_session *s)
{(void)b;if(!fail_release)s->npa_clients[0]=0;}
int sm8750_loader_avdd_enable(efi_boot_services *b,int en,struct sm8750_avdd_session *s)
{(void)b;assert(en);s->owned=1;return fail_avdd?-1:0;}
int sm8750_loader_avdd_restore(efi_boot_services *b,struct sm8750_avdd_session *s)
{(void)b;if(fail_release)return -1;s->owned=0;return 0;}
static void reset(void) {
 regs[0]=0x400;regs[1]=3;regs[2]=0x440;regs[3]=1;
 writes=reads=fail_write=fail_after=delay=fail_power=fail_avdd=fail_release=drop_write=0;
}
int main(void) {
 efi_boot_services bs={0};bs.stall=stall;
 struct sm8750_panel_io io={0,rd,wr};struct sm8750_panel s={0};
 reset();assert(sm8750_panel_prepare(&s,&bs,&io,0)==-3&&!reads&&!writes);
 regs[0]|=0x1000;assert(sm8750_panel_prepare(&s,&bs,&io,1)==-3&&!writes);
 reset();assert(!sm8750_panel_prepare(&s,&bs,&io,1));
 assert(s.prepared&&delay==290000&&regs[0]==0x6c0&&regs[2]==0x4c0&&regs[1]==3);
 assert(sm8750_panel_prepare(&s,&bs,&io,1)==-6);
 assert(!sm8750_panel_release(&s)&&!s.prepared&&!s.pins_owned&&!s.avdd.owned);
 assert(regs[0]==0x400&&regs[1]==3&&regs[2]==0x440);
 for(unsigned n=1;n<=4;n++) {
  reset();memset(&s,0,sizeof(s));fail_write=n;
  assert(sm8750_panel_prepare(&s,&bs,&io,1)==-1);
  assert(!s.pins_owned&&!s.avdd.owned&&!s.power.npa_clients[0]);
  assert(regs[0]==0x400&&regs[1]==3&&regs[2]==0x440);
 }
 reset();memset(&s,0,sizeof(s));fail_after=2;
 assert(sm8750_panel_prepare(&s,&bs,&io,1)==-1&&s.cleanup_error);
 assert(s.pins_owned&&s.avdd.owned&&s.power.npa_clients[0]);
 fail_after=0;assert(!sm8750_panel_release(&s));
 reset();memset(&s,0,sizeof(s));drop_write=2;
 assert(sm8750_panel_prepare(&s,&bs,&io,1)==-1&&!s.prepared&&!s.pins_owned);
 reset();memset(&s,0,sizeof(s));fail_power=fail_release=1;
 assert(sm8750_panel_prepare(&s,&bs,&io,1)==-1&&s.power.npa_clients[0]);
 assert(!writes);fail_release=0;assert(!sm8750_panel_release(&s));
 reset();memset(&s,0,sizeof(s));fail_avdd=fail_release=1;
 assert(sm8750_panel_prepare(&s,&bs,&io,1)==-1&&s.avdd.owned);
 assert(!writes);fail_release=0;assert(!sm8750_panel_release(&s));
 puts("PASS: opt-in, ownership gate, exact delays, pin readbacks, every write failure, retained cleanup/retry");
}

void op13_trace(uint32_t s,uint32_t d){(void)s;(void)d;}
