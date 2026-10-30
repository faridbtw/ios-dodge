#include <assert.h>
#include <stdio.h>
#include "op13_peach_wake.h"
struct fake { uint32_t window,original,wake; unsigned reads,delay,ready_after; int select_fail,restore_fail,invalid; };
static uint32_t rd(void *ctx,unsigned off)
{
 struct fake *f=ctx;
 if(off==0x3278)return f->invalid?~0u:f->window;
 assert(off==0x84040 && f->wake==1 && (f->window&0x3f)==0x3c);
 f->reads++;return f->reads>f->ready_after?2:1;
}
static void wr(void *ctx,unsigned off,uint32_t value)
{
 struct fake *f=ctx;
 if(off==0x3004){assert(value<=1);f->wake=value;return;}
 assert(off==0x3278);
 if(f->select_fail && value!=f->original)return;
 if(f->restore_fail && value==f->original)return;
 f->window=value;
}
static void pause_ms(void *ctx,unsigned ms){struct fake *f=ctx;assert(ms==5);f->delay+=ms;}
int main(void)
{
 const struct op13_peach_wake_ops ops={rd,wr,pause_ms};
 struct op13_peach_wake_result r;
 for(unsigned delay=0;delay<=10;delay++){
  struct fake f={.window=0x81234517,.original=0x81234517,.ready_after=delay};
  assert(op13_peach_wake_umac(&ops,&f,&r)==0);
  assert(f.window==f.original && f.wake==1 && r.after==2 && f.delay==delay*5);
 }
 struct fake f={.window=0x40000002,.original=0x40000002,.ready_after=100};
 assert(op13_peach_wake_umac(&ops,&f,&r)==-4);
 assert(f.window==f.original && f.wake==0 && f.delay==50 && r.polls==11);
 f=(struct fake){.window=0x40000002,.original=0x40000002,.select_fail=1};
 assert(op13_peach_wake_umac(&ops,&f,&r)==-3 && f.wake==0 && f.reads==0);
 f=(struct fake){.window=0x40000002,.original=0x40000002,.restore_fail=1};
 assert(op13_peach_wake_umac(&ops,&f,&r)==-5 && f.wake==0);
 f=(struct fake){.invalid=1};
 assert(op13_peach_wake_umac(&ops,&f,&r)==-2 && f.wake==0 && f.reads==0);
 assert(op13_peach_wake_umac(NULL,&f,&r)==-1);
 puts("Native UMAC wake: bounded polls, exact offsets, window restoration and failure rollback passed.");
}
