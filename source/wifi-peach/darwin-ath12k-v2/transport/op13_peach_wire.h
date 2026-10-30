/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef OP13_PEACH_WIRE_H
#define OP13_PEACH_WIRE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "op13_wifi_qrtr.h"
/* QRTR length excludes padding; the MHI transfer includes zero padding to4. */
static size_t op13_peach_qrtr_encode(uint8_t *out,size_t capacity,unsigned type,
 uint32_t from_port,uint32_t node,uint32_t port,const void *data,size_t n)
{
 if(!out||!data||!n||n>8192||capacity<32||n>capacity-32)return 0;
 size_t total=32+((n+3)&~(size_t)3);if(total>capacity)return 0;
 memset(out,0,total);op13_qrtr_put(out,1);op13_qrtr_put(out+4,type);
 op13_qrtr_put(out+8,1);op13_qrtr_put(out+12,from_port);op13_qrtr_put(out+20,(uint32_t)n);
 op13_qrtr_put(out+24,node);op13_qrtr_put(out+28,port);memcpy(out+32,data,n);return total;
}
#endif
