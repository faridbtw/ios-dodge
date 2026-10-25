/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../loader/sm8750_avdd.c"
static uint8_t regs[256]; static unsigned calls,writes,fail_write;
static uint32_t read_fn(struct avdd_spmi *p,uint32_t bus,uint32_t sid,uint32_t pri,
 uint32_t addr,uint8_t *v,uint32_t n,uint32_t *done)
{ (void)p; calls++; assert(!bus&&sid==9&&!pri&&addr>=0x8a00&&addr+n<=0x8b00);
 memcpy(v,regs+(addr-0x8a00),n); *done=n; return 0; }
static uint32_t write_fn(struct avdd_spmi *p,uint32_t bus,uint32_t sid,uint32_t pri,
 uint32_t addr,const uint8_t *v,uint32_t n)
{ (void)p; calls++; writes++; assert(!bus&&sid==9&&!pri&&n==1);
 assert(addr==0x8a40||addr==0x8a44||addr==0x8a46);
 if(fail_write==writes) return 1;
 regs[addr-0x8a00]=*v; return 0; }
static struct avdd_spmi sp={0x10003,read_fn,write_fn};
static efi_status loc(const efi_guid *g,void *r,void **p)
{ (void)g;(void)r; calls++; *p=&sp; return 0; }
int main(void)
{
 efi_boot_services bs={0}; bs.locate_protocol=loc;
 struct sm8750_avdd_session s={0};
 assert(sm8750_loader_avdd_enable(&bs,0,&s)==-3&&!calls);
 regs[4]=0x10; regs[5]=0x11;
 assert(sm8750_loader_avdd_enable(&bs,1,&s)==-3&&!writes);
 regs[0x41]=1; regs[0x42]=4; regs[0x45]=1;
 assert(sm8750_loader_avdd_enable(&bs,1,&s)==0&&writes==3&&s.owned);
 assert(regs[0x40]==1&&regs[0x44]==0x80&&regs[0x46]==0x80);
 assert(sm8750_loader_avdd_restore(&bs,&s)==0&&!s.owned);
 assert(!regs[0x40]&&!regs[0x44]&&!regs[0x46]);
 regs[0x40]=1;assert(sm8750_loader_avdd_enable(&bs,1,&s)==-3);regs[0x40]=0;regs[0x44]=0x80;assert(sm8750_loader_avdd_enable(&bs,1,&s)==-3);regs[0x44]=0;
 writes=0; fail_write=2;
 assert(sm8750_loader_avdd_enable(&bs,1,&s)==-1&&!s.owned);
 assert(!regs[0x40]&&!regs[0x44]&&!regs[0x46]);
 puts("PASS: AVDD disabled/config gates, exact address allowlist, readback and rollback");
}

void op13_trace(uint32_t s,uint32_t d){(void)s;(void)d;}
