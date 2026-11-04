/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef OP13_TRACE_INFO_H
#define OP13_TRACE_INFO_H
#include <stdint.h>
/* 40 wire bytes; trailing NUL is not sent. Fits a 64-byte fastboot response. */
static unsigned op13_trace_info(char out[48],uint32_t stage,uint32_t detail)
{
 const char prefix[]="INFOtouch stage=";
 const char middle[]=" detail=";
 unsigned n=0;
 for(unsigned i=0;i<sizeof(prefix)-1;i++)out[n++]=prefix[i];
 for(unsigned i=0;i<8;i++)out[n++]="0123456789abcdef"[(stage>>(28-i*4))&15];
 for(unsigned i=0;i<sizeof(middle)-1;i++)out[n++]=middle[i];
 for(unsigned i=0;i<8;i++)out[n++]="0123456789abcdef"[(detail>>(28-i*4))&15];
 out[n]=0;return n;
}
#endif
