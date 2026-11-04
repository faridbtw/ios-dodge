/* SPDX-License-Identifier: BSD-2-Clause
 * Independent wire encoding of Linux ath12k dp_htt.[ch] version exchange. */
#ifndef OP13_WIFI_HTT_H
#define OP13_WIFI_HTT_H
#include "op13_wifi_qrtr.h"
static size_t op13_htt_version_request(uint8_t *b,size_t n)
{
 if(!b||n<8)return 0;
 op13_qrtr_put(b,0);op13_qrtr_put(b+4,5u|(4u<<8)|(2u<<16));return 8;
}
static int op13_htt_version_response(const uint8_t *b,size_t n,unsigned *major,unsigned *minor)
{
 if(!b||!major||!minor||(n<4||n>2040||(n&3)))return -1;
 uint32_t v=op13_qrtr_u32(b);if((v&255)||(v>>24))return -2;
 /* ath12k reads the fixed first word; appended option TLVs are not
  * used by this version-only probe and are not interpreted as ring data. */
 unsigned hi=(v>>16)&255,lo=(v>>8)&255;if(hi!=3)return -3;
 *major=hi;*minor=lo;return 0;
}
#endif
