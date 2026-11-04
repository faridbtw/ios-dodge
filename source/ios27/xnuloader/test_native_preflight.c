#include <assert.h>
#include "native_preflight.c"
static unsigned n;static uint32_t values[8]={0x100,0,0,0,0,0,0,0};
static int rd(void*c,uint32_t o,uint32_t*v){(void)c;static const uint32_t offsets[]={0x68,0x40,0x800,0x804,0x614,0x644,0xe1c,0xe18};assert(o==offsets[n]);*v=values[n++];return 0;}
int main(void){struct native_lease l={1,0xa90000,1,1,1,1,1};struct native_snapshot s;
assert(!native_preflight(&l,1,rd,0,&s)&&n==8);n=0;l.closed=0;assert(native_preflight(&l,1,rd,0,&s)==-3&&!n);l.closed=1;
for(unsigned i=1;i<8;i++){values[i]=1;n=0;assert(native_preflight(&l,1,rd,0,&s)==-6);values[i]=0;}
n=0;values[0]=0x200;assert(native_preflight(&l,1,rd,0,&s)==-6);}
