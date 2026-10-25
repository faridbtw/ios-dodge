#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "fifo_snapshot.h"
static unsigned calls,fail,mutate,busy;
static int rd(void*c,uint64_t a,uint32_t*v){(void)c;unsigned i=calls%FIFO_SNAPSHOT_COUNT;assert(a==0xa90000u+fifo_snapshot_offset(i));calls++;if(calls==fail)return -1;*v=i==2?0x102:0;if(i==1||i==15)*v=1;if(busy&&i==13)*v=1;if(mutate&&calls==FIFO_SNAPSHOT_COUNT+1)*v=1;return 0;}
int main(void){struct fifo_snapshot r;assert(fifo_snapshot_read(0,rd,0,&r)==-3&&!calls);
 assert(!fifo_snapshot_read(1,rd,0,&r)&&r.completed==32&&r.local_idle&&r.protocol_spi&&r.stable);
 for(unsigned n=1;n<=32;n++){calls=0;fail=n;assert(fifo_snapshot_read(1,rd,0,&r)==-1&&r.completed==n-1&&!r.local_idle);}
 calls=fail=0;mutate=1;assert(!fifo_snapshot_read(1,rd,0,&r)&&!r.stable&&!r.local_idle);
 calls=mutate=0;busy=1;assert(!fifo_snapshot_read(1,rd,0,&r)&&!r.local_idle);
 assert(fifo_snapshot_offset(16)==UINT32_MAX);
 puts("PASS actual snapshot: fixed allowlist, all32 read failures, unstable and activeDMA refusal flags; no write API");
}
