#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "sm8750_first_read.c"
void op13_trace(uint32_t s,uint32_t d){(void)s;(void)d;}
static int scenario,opens,transfers,closes,preps,releases;
int sm8750_loader_spi_inspect(efi_boot_services*b,int e,const void*i,size_t z,struct sm8750_spi_topology*r){(void)b;(void)e;(void)i;(void)z;(void)r;return -6;}
int sm8750_loader_config_query(efi_boot_services*b,int e,struct sm8750_config_query_report*r){(void)b;(void)e;memset(r,0,sizeof(*r));r->count0=10;r->count1=8;r->se_wrapper_base=0xa80000;r->se_index=4;r->derived_base=0xa90000;r->derived_matched_a90000=1;r->wrapper_common_base=0xac0000;if(scenario==1)r->count0=8;return -6;}
static uint32_t op(uint32_t n,void**h){assert(n==15);opens++;*h=(void*)1;return scenario==2?2:0;}
static uint32_t tr(void*h,const struct first_config*c,const uint8_t*t,uint32_t n,uint8_t*r,uint32_t m){assert(h&&n==m&&c->max_hz==1000000&&c->bits==8&&!c->cs_mode);for(unsigned i=0;i<n;i++)assert(t[i]==255);transfers++;memset(r,0,n);if(n==4){r[0]=scenario==3?0:0xa5;r[1]=0x10;r[2]=24;}else if(scenario!=7){r[0]=0xa5;r[1]=3;r[2]=1;r[3]=1;r[24]=0;r[25]=2;memset(r+26,0x5a,5);}return scenario==4?4:0;}
static uint32_t cl(void*h){assert(h);closes++;return scenario==6?6:0;}
static struct first_spi spi={0x10000,op,tr,cl};
static int32_t path(struct flag_node*n,const char*s){(void)n;assert(strstr(s,"_SE_4"));return 0;}
static int32_t prop(const struct flag_node*n,const char*s,uint32_t*v){(void)n;assert(!strcmp(s,"se_flags"));*v=scenario==5?0:8;return 0;}
static void *cfg[21];
static efi_status loc(const efi_guid*g,void*x,void**p){(void)x;*p=g->a==0x4c7ffd28?(void*)&spi:(void*)cfg;return 0;}
static int pre(void*x){(void)x;preps++;return 0;}static int rel(void*x){(void)x;releases++;return 0;}
int main(void){cfg[6]=(void*)path;cfg[20]=(void*)prop;efi_boot_services bs={.locate_protocol=loc};struct sm8750_first_read_report r;struct sm8750_first_read_session session={.limit=5};
 assert(sm8750_first_read(&bs,0,0,0,0,pre,rel,0,&session,&r)==-3&&!opens);
 /* Missing callbacks must refuse before power preparation or protocol calls. */
 for(unsigned missing=0;missing<3;missing++){
  struct first_spi saved=spi;
  if(missing==0)spi.open=0;else if(missing==1)spi.transfer=0;else spi.close=0;
  assert(sm8750_first_read(&bs,1,0,0,0,pre,rel,0,&session,&r)==-3);
  assert(!opens&&!preps&&!session.handle&&!session.panel_owned);
  spi=saved;
 }
 for(scenario=0;scenario<6;scenario++){opens=transfers=closes=preps=releases=0;int rc=sm8750_first_read(&bs,1,0,0,0,pre,rel,0,&session,&r);if(scenario==1||scenario==5){assert(rc==-3&&!opens&&!preps);continue;}assert(opens==1&&closes==1&&preps==1&&releases==1);if(!scenario)assert(!rc&&transfers==2&&r.raw_transfer_complete&&r.frame_valid&&r.identify_valid&&r.packet_status==0);if(scenario==2)assert(transfers==0&&rc);if(scenario==3)assert(transfers==1&&rc==-6);if(scenario==4)assert(transfers==1&&rc);}
 scenario=7;assert(sm8750_first_read(&bs,1,0,0,0,pre,rel,0,&session,&r)==-2);assert(r.raw_transfer_complete&&!r.frame_valid&&!r.identify_valid&&r.packet_status==-2&&!session.panel_owned&&!session.handle);
 scenario=6;opens=transfers=closes=preps=releases=0;
 assert(sm8750_first_read(&bs,1,0,0,0,pre,rel,0,&session,&r)==-1);
 assert(session.handle&&session.blocked&&session.panel_owned&&!releases);
 assert(sm8750_first_read(&bs,1,0,0,0,pre,rel,0,&session,&r)==-6&&opens==1);
 assert(sm8750_first_read_cleanup(&bs,rel,0,&session,&r)==-1&&!releases);
 scenario=0;assert(!sm8750_first_read_cleanup(&bs,rel,0,&session,&r));
 assert(!session.handle&&!session.blocked&&!session.panel_owned&&releases==1);
 scenario=0;for(unsigned stage=1;stage<=5;stage++){
 session.limit=stage;opens=transfers=closes=preps=releases=0;
 assert(!sm8750_first_read(&bs,1,0,0,0,pre,rel,0,&session,&r));
 assert(preps==(stage>=2)&&opens==(stage>=3)&&transfers==(stage>=5?2:stage>=4?1:0));
 }
 puts("PASS first-read: gating, partial-open cleanup, exact FF lengths/config, header refusal, failure rollback");}
