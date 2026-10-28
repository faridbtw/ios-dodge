#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "op13_dsi_brightness.h"
struct fake {uint32_t regs[128],fifo[2];unsigned nf,writes;int complete;};
static uint32_t rd(void *c,unsigned o){assert(o%4==0&&o<512);return ((struct fake*)c)->regs[o/4];}
static void wr(void *c,unsigned o,uint32_t v){struct fake *f=c;f->writes++;
 const unsigned allowed[]={4,0x3c,0x4c,0x84,0x90,0x110,0x15c,0x17c,0x1ec};int ok=0;for(unsigned i=0;i<9;i++)if(o==allowed[i])ok=1;assert(ok);
 if(o==0x17c){assert(f->nf<2);f->fifo[f->nf++]=v;}
 if(o==0x110)f->regs[o/4]=(f->regs[o/4]&OP13_DSI_STATUS_BITS&~v)|(v&~OP13_DSI_STATUS_BITS);
 else f->regs[o/4]=v;
 if(o==0x90&&f->complete)f->regs[0x110/4]|=1;
}
static void delay(void *c,unsigned us){(void)c;(void)us;}
static void init(struct fake *f){memset(f,0,sizeof(*f));f->regs[1]=0x107;f->regs[0x3c/4]=0x14000000;f->regs[0x84/4]=0x80020040;f->regs[0x4c/4]=32;f->regs[0x110/4]=(1<<16)|2;f->complete=1;}
int main(void){struct fake f;struct op13_dsi_io io={&f,rd,wr,delay};init(&f);
 assert(op13_dsi_payload(703)==0xffbf0251u&&op13_dsi_payload(2461)==0xff9d0951u);
 assert(op13_dsi_set(&io,0)==-2&&!f.writes);assert(op13_dsi_set(&io,3516)==-2&&!f.writes);
 assert(!op13_dsi_set(&io,703));assert(f.nf==2&&f.fifo[0]==0xc0390003&&f.fifo[1]==0xffbf0251);
 assert(f.regs[1]==0x107&&f.regs[0x3c/4]==0x14000000&&f.regs[0x4c/4]==32&&f.regs[0x84/4]==0x80020040);
 assert(f.regs[0x110/4]&(1<<16));assert(f.regs[0x110/4]&2);assert(!f.regs[0x15c/4]);
 init(&f);f.regs[0x15c/4]=1;assert(op13_dsi_set(&io,703)==-3&&!f.writes);
 init(&f);f.regs[2]=4;assert(op13_dsi_set(&io,703)==-1&&!f.writes);
 init(&f);f.regs[0x110/4]|=1<<24;assert(op13_dsi_set(&io,703)==-4&&!f.writes);
 init(&f);f.complete=0;assert(op13_dsi_set(&io,703)==-5);
 puts("PASS DCS51 packet, native normal-range bounds, W1C preservation, saved-state restoration, busy/error/timeout rejection");}
