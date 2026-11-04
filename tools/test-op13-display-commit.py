#!/usr/bin/env python3
"""Run the real framebuffer commit policy under ASAN/UBSAN with synthetic TE."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
header=ROOT/'experimental/display/anti-tear-v2/xnu-src/iokit/Kernel'
source=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "MiniFramebufferCommit.h"
struct clock {uint64_t now,period;int stuck;unsigned reads,sleeps;};
static uint64_t now(void*p){struct clock*c=p;return c->now+=1000;}
static unsigned level(void*p){struct clock*c=p;c->reads++;return c->stuck>=0?(unsigned)c->stuck:(c->now%c->period)<60000;}
static void sleep_until(void*p,uint64_t t){struct clock*c=p;assert(t>=c->now);c->now=t;c->sleeps++;}
static void copy(void*a,const void*b,size_t n){memcpy(a,b,n);}
int main(void){
 unsigned cases=0;
 for(unsigned hz=48;hz<=144;hz+=12){
  struct clock c={100000,1000000000ULL/hz,-1,0,0};struct mini_fb_te state={0,0,0,1};
  struct mini_fb_te_io io={&c,now,level,sleep_until};
  assert(mini_fb_wait_te(&state,&io));assert(state.period>=c.period-2000&&state.period<=c.period+2000);
  for(unsigned f=0;f<100;f++){
   c.now+=2300000;uint64_t before=c.now;assert(mini_fb_wait_te(&state,&io));
   assert(c.now-before<25002000);assert(c.now%c.period<70000);cases++;
  }
  assert(c.sleeps);assert(!state.failures);
  assert(mini_fb_te_copy_ready(&state,state.last));
  assert(mini_fb_te_copy_ready(&state,state.last+150000));
  assert(!mini_fb_te_copy_ready(&state,state.last+150001));
  assert(!mini_fb_te_copy_ready(&state,state.last-1));
  /* A post-edge scheduling delay must retain the previous frame and retry. */
  c.now+=1000000;assert(!mini_fb_te_copy_ready(&state,c.now));
  assert(mini_fb_wait_te(&state,&io));assert(mini_fb_te_copy_ready(&state,c.now));cases+=6;
 }
 for(int stuck=0;stuck<=1;stuck++){
  struct clock c={0,8333333,stuck,0,0};struct mini_fb_te state={0,0,0,1};struct mini_fb_te_io io={&c,now,level,sleep_until};
  for(unsigned i=0;i<3;i++){uint64_t before=c.now;assert(!mini_fb_wait_te(&state,&io));assert(c.now-before<=50003000);}
  assert(!state.enabled);assert(!mini_fb_te_copy_ready(&state,c.now));unsigned reads=c.reads;assert(!mini_fb_wait_te(&state,&io));assert(reads==c.reads);cases++;
 }
 for(unsigned width=1;width<=127;width+=7)for(unsigned height=1;height<=43;height+=3){
  unsigned stride=width*4+16;size_t n=(size_t)stride*height;unsigned char*a=malloc(n+32),*b=malloc(n+32);assert(a&&b);
  memset(a,0xa5,n+32);memset(b,0x3c,n+32);
  assert(mini_fb_commit_rows(a+16,b+16,width,height,stride,copy));
  for(unsigned y=0;y<height;y++){for(unsigned x=0;x<width*4;x++)assert(a[16+y*stride+x]==0x3c);for(unsigned x=width*4;x<stride;x++)assert(a[16+y*stride+x]==0xa5);}
  for(unsigned i=0;i<16;i++)assert(a[i]==0xa5&&a[n+16+i]==0xa5);
  assert(!mini_fb_commit_rows(a+16,a+16,width,height,stride,copy));assert(!mini_fb_commit_rows(a,b,width,height,width*4-1,copy));
  free(a);free(b);cases++;
 }
 assert(!mini_fb_commit_rows(0,0,0,0,0,copy));
 printf("%u frame-copy and TE timing cases passed\n",cases);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(source)
 subprocess.run(['/usr/bin/clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(header),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
