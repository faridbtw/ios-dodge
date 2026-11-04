#include "trace_info.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void) {
 struct { unsigned char before[8]; char data[48]; unsigned char after[8]; } b;
 memset(&b,0xa5,sizeof b);
 unsigned n=op13_trace_info(b.data,0xffffffff,0x80000000);
 assert(n==40 && strcmp(b.data,"INFOtouch stage=ffffffff detail=80000000")==0);
 for(unsigned i=0;i<8;i++)assert(b.before[i]==0xa5 && b.after[i]==0xa5);
 assert(op13_trace_info(b.data,0,1)==40);
 assert(strcmp(b.data,"INFOtouch stage=00000000 detail=00000001")==0);
 puts("fastboot INFO formatting/bounds passed");
}
