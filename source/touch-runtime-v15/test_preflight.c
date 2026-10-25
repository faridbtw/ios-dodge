#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "geni_preflight.c"
void op13_trace(uint32_t s,uint32_t d){(void)s;(void)d;}
static unsigned get_calls,en_calls,on_calls,dis_calls,read_calls;
static unsigned fail_id,fail_en,fail_on,off,fail_dis,fail_read,land_error;
static unsigned refs[5],base[5];
static unsigned order[5];
static efi_status bad(unsigned x,unsigned n){return x==n?((uint64_t)1<<63):0;}
static efi_status id(struct clock*p,const char*n,uint64_t*v){(void)p;unsigned i=get_calls++;assert(!strcmp(n,names[i]));*v=i;return bad(i+1,fail_id);}
static efi_status en(struct clock*p,uint64_t n){(void)p;en_calls++;if(bad(n+1,fail_en)){if(land_error)refs[n]++;return (uint64_t)1<<63;}refs[n]++;return 0;}
static efi_status on(struct clock*p,uint64_t n,uint8_t*v){(void)p;on_calls++;*v=!off||off!=n+1;return bad(n+1,fail_on);}
static efi_status dis(struct clock*p,uint64_t n){(void)p;order[dis_calls++%5]=(unsigned)n;if(bad(n+1,fail_dis))return (uint64_t)1<<63;assert(!base[n]);if(refs[n])refs[n]--;return 0;}
static efi_status enabled(struct clock*p,uint64_t n,uint8_t*v){(void)p;*v=refs[n]!=0;return 0;}
static struct clock clockp={0x1000b,id,en,dis,enabled,on};
static efi_status loc(const efi_guid*g,void*x,void**p){(void)x;assert(g->a==guid.a);*p=&clockp;return 0;}
static int rd(void*c,uint64_t a,uint32_t*v){(void)c;assert(a==0xa90000u+fifo_snapshot_offset(read_calls%FIFO_SNAPSHOT_COUNT));for(unsigned i=0;i<5;i++)assert(refs[i]==(base[i]?base[i]:1));read_calls++;*v=a==0xa90064?1:0x100;return read_calls==fail_read?-1:0;}
static void reset(void){get_calls=en_calls=on_calls=dis_calls=read_calls=0;fail_id=fail_en=fail_on=off=fail_dis=fail_read=land_error=0;for(unsigned i=0;i<5;i++)refs[i]=base[i]=0;}
static void restored(void){for(unsigned i=0;i<5;i++)assert(refs[i]==base[i]);}
int main(void){efi_boot_services bs={.locate_protocol=loc};struct geni_preflight s={0};reset();assert(geni_preflight_run(&bs,0,rd,0,&s)==-3&&!get_calls);
 assert(!geni_preflight_run(&bs,1,rd,0,&s)&&s.reads==32&&!s.votes);restored();for(unsigned i=0;i<5;i++)assert(order[i]==4-i);
 reset();memset(&s,0,sizeof(s));for(unsigned i=0;i<5;i++)refs[i]=base[i]=1;assert(!geni_preflight_run(&bs,1,rd,0,&s)&&!en_calls&&!dis_calls);restored();
 for(unsigned n=1;n<=5;n++)for(unsigned kind=0;kind<4;kind++){
  reset();memset(&s,0,sizeof(s));if(!kind)fail_id=n;else if(kind==1)fail_en=n;else if(kind==2)fail_on=n;else off=n;
  assert(geni_preflight_run(&bs,1,rd,0,&s)==-1&&!read_calls&&!s.votes);restored();
 }
 for(unsigned n=1;n<=32;n++){reset();memset(&s,0,sizeof(s));fail_read=n;assert(geni_preflight_run(&bs,1,rd,0,&s)==-1&&!s.votes);restored();}
 for(unsigned n=1;n<=5;n++){reset();memset(&s,0,sizeof(s));fail_dis=n;assert(geni_preflight_run(&bs,1,rd,0,&s)==-1&&s.votes==(1u<<(n-1)));assert(geni_preflight_run(&bs,1,rd,0,&s)==-6);fail_dis=0;assert(!geni_preflight_release(&bs,&s)&&!s.votes);restored();}
 for(unsigned n=1;n<=5;n++){
 reset();memset(&s,0,sizeof(s));fail_en=n;land_error=1;
 assert(geni_preflight_run(&bs,1,rd,0,&s)==-1&&!s.votes&&!read_calls);restored();assert(dis_calls==n);
 reset();memset(&s,0,sizeof(s));fail_en=fail_dis=n;land_error=1;
 assert(geni_preflight_run(&bs,1,rd,0,&s)==-1&&s.votes==(1u<<(n-1))&&refs[n-1]==1);
 assert(geni_preflight_run(&bs,1,rd,0,&s)==-6);fail_dis=0;assert(!geni_preflight_release(&bs,&s));restored();
 }
 puts("PASS actual module: fixed addresses/order, shared initial refs preserved, every ID/enable/on/read/disable failure, retained-vote retry");
}
