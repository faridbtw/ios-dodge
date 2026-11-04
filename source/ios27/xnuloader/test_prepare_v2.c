#include <assert.h>
#include <string.h>
#include "sm8750_prepare.c"
static int ids,rates,states,reads;
static efi_status id(struct clock_protocol*p,const char*n,uint64_t*v){(void)p;(void)n;*v=++ids;return 0;}
static efi_status rate(struct clock_protocol*p,uint64_t i,uint32_t*v){(void)p;(void)i;(void)v;rates++;return EFI_ERROR_BIT|19;}
static efi_status on(struct clock_protocol*p,uint64_t i,uint8_t*v){(void)p;(void)i;states++;*v=0;return 0;}
static uint32_t readsp(struct spmi_protocol*p,uint32_t b,uint32_t s,uint32_t q,uint32_t a,uint8_t*v,uint32_t n,uint32_t*r){(void)p;assert(!b&&s==9&&!q);reads++;memset(v,0,n);if(a==0x8a04)v[0]=0x10;*r=n;return 0;}
static struct clock_protocol c={.revision=0x1000b,.get_id=id,.is_on=on,.get_frequency=rate};
static struct spmi_protocol s={.revision=0x10003,.read_long=readsp};
static efi_status loc(const efi_guid*g,void*x,void**p){(void)x;*p=g->a==clock_guid.a?(void*)&c:(void*)&s;return 0;}
int main(void){efi_boot_services bs={.locate_protocol=loc};struct sm8750_prepare_report r={0};assert(sm8750_loader_prepare(&bs,1,0,&r)==-1);assert(ids==3&&rates==3&&states==3&&reads==2&&!r.clock_votes);for(int i=0;i<3;i++)assert(!r.clock_id_status[i]&&EFI_ERROR(r.clock_frequency_status[i])&&!r.clock_on_status[i]);c.revision=0;ids=0;assert(sm8750_loader_prepare(&bs,1,0,&r)==-1);assert(!ids&&reads==4);}
