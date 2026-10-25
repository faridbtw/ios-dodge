#!/usr/bin/env python3
"""Check the production clock transition against vendor constants and failure states."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
text=(ROOT/'source/xnu-build-snapshot/xnu-8796.121.2/bsd/dev/adreno8_cx.c').read_text()
production=text[text.index('/* Dodge operating-clock experiment.'):]
source=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
struct adreno8_block { const char *name; uintptr_t base; };
static uint32_t gpu[0x9800/4], gcc[0x60000/4];
static struct adreno8_block adreno8_gpucc={"gpucc",(uintptr_t)gpu},adreno8_gcc={"gcc",(uintptr_t)gcc};
#define GPUCC_GMU_RCGR 0x9318
#define GPUCC_HUB_RCGR 0x93ec
#define RCG_SOURCE_MASK 0x371f
static bool adreno8_reset_required=true;
static unsigned scenario,writes,pll_writes,source_calls;
static int adreno8_say(const char *s,...){(void)s;return 0;}
#define printf adreno8_say
static void adreno8_announce(const char *s,...){(void)s;}
static void adreno8_settle(void){}
static void adreno8_sleep_us(uint32_t n){assert(n==10);}
static bool adreno8_cx_ready(void){return scenario!=13;}
static uint32_t adreno8_rd(uintptr_t a){
 assert((a>=(uintptr_t)gpu && a<(uintptr_t)(gpu+sizeof(gpu)/4)) ||
        (a>=(uintptr_t)gcc && a<(uintptr_t)(gcc+sizeof(gcc)/4)));
 return *(uint32_t*)a;
}
static void adreno8_wr(uintptr_t a,uint32_t value){
 writes++;
 if(a>=(uintptr_t)gcc && a<(uintptr_t)(gcc+sizeof(gcc)/4)){
  assert(a==(uintptr_t)gcc+0x52000 && value==(0x123|1u<<15));
  if(scenario!=8)*(uint32_t*)a=value;return;
 }
 assert(a>=(uintptr_t)gpu && a<(uintptr_t)(gpu+sizeof(gpu)/4));
 unsigned off=a-(uintptr_t)gpu;pll_writes++;
 if(off==0 && (value&1) && scenario==6)value &= ~1u;
 *(uint32_t*)a=value;
 if(off==4 && value==1 && scenario!=4){
  gpu[0]=(1u<<31)|4;
  if(scenario==5)gpu[0]|=0x100;
 }
}
static bool adreno8_wait_for(struct adreno8_block *b,uint32_t offset,uint32_t mask,uint32_t value,uint32_t timeout){
 assert(b==&adreno8_gpucc && !offset && mask==(1u<<31) && value==mask && timeout==10);
 return scenario!=4;
}
static bool adreno8_clock_source(const char *name,uint32_t offset,uint32_t value){
 (void)name;source_calls++;
 assert(gpu[0]==((1u<<31)|5) && gpu[0x18/4]==0x407);
 if(offset==0x9318){assert(value==0x201 && source_calls==1);if(scenario==7)return false;}
 else {assert(offset==0x93ec && value==0x505 && (gcc[0x52000/4]&(1u<<15)));if(scenario==9)return false;}
 gpu[(offset+4)/4]=0x100000|value;return true;
}
'''
tests=r'''
#undef printf
int main(int argc,char **argv){
 assert(argc==2);scenario=atoi(argv[1]);
 gcc[0]=(1u<<31)|(1u<<30)|5;gcc[1]=1;gcc[4]=31;gcc[5]=0x4000;gcc[0x52000/4]=0x123;
 gpu[0]=0x100;gpu[4]=0x440000;gpu[0x18/4]=1;
 gpu[(GPUCC_GMU_RCGR+4)/4]=gpu[(GPUCC_HUB_RCGR+4)/4]=0x100000;
 if(scenario==1)gcc[0]&=~(1u<<31);
 if(scenario==2)gcc[4]=34;
 if(scenario==3 || scenario==10 || scenario==11 || scenario==14){
  gpu[0]=(1u<<31)|5;gpu[1]=1;gpu[4]=0x44440034;gpu[5]=0x1555;gpu[6]=0x407;
  if(scenario==3)gpu[4]=0x44440040;
  if(scenario==11)gpu[0]&=~1u;
  if(scenario==14)gpu[6]=7;
 }
 if(scenario==15)gcc[0]&=~(1u<<30);
 if(scenario==16)gcc[1]=0; /* Shared PLL remains active in firmware vote mode. */
 if(scenario==12)adreno8_reset_required=false;
 assert(adreno8_operating_clocks()==(scenario==0 || scenario==10 || scenario==16));
 if(scenario==0 || scenario==10 || scenario==16){
  assert(source_calls==2 && gpu[(0x9318+4)/4]==0x100201 && gpu[(0x93ec+4)/4]==0x100505);
  if(scenario==0 || scenario==16){
   assert(gpu[4]==0x44440034 && gpu[5]==0x1555 && gpu[6]==0x407 && gpu[7]==2);
   assert(gpu[8]==0x19660387 && gpu[9]==0x098060a0 && gpu[10]==0xb416cb20);
  }else assert(pll_writes==0);
 } else if(scenario<=3 || scenario>=11)assert(writes==0 && source_calls==0);
 else if(scenario<=6)assert(source_calls==0);
 else if(scenario<=8)assert(source_calls==1 && gpu[(0x93ec+4)/4]==0x100000);
 /* The shared CPU/system PLL has never been modified. */
 assert(gcc[1]==(scenario==16?0:1) && gcc[4]==(scenario==2?34:31) && gcc[5]==0x4000);
 puts("operating clock success/failure scenario passed");
}
'''
with tempfile.TemporaryDirectory(prefix='op13-clocks-') as directory:
 p=Path(directory)/'check.c';p.write_text(source+production+tests);binary=Path(directory)/'check'
 subprocess.run(['cc','-std=c11','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p),'-o',str(binary)],check=True)
 for scenario in range(17):subprocess.run([str(binary),str(scenario)],check=True)
 print('Vendor clock values, PLL calibration/output checks, shared-parent preservation and failure retention: PASS')
