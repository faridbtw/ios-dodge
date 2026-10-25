/* SPDX-License-Identifier: BSD-2-Clause
 * Bounded no-extension subset of touch/src/s3910.c:s3910_probe. Wire layout
 * comes from pinned OnePlus TouchCom v1 continued_read/update_crc/startup.
 * CRC-bearing and extra-RC-bearing variants are deliberately unsupported.
 */
#ifndef OP13_STARTUP_PACKET_H
#define OP13_STARTUP_PACKET_H
static int startup_packet_validate(const uint8_t *h,size_t hn,
 const uint8_t *b,size_t bn,unsigned *frame_valid)
{
 if(!frame_valid)return -5;
 *frame_valid=0;
 if(!h||!b||hn!=4||bn!=31)return -5;
 if(h[0]!=0xa5||h[1]!=0x10||h[2]!=24||h[3])return -2;
 if(b[0]!=0xa5||b[1]!=3||b[26]!=0x5a)return -2;
 *frame_valid=1;
 /* update_crc extracts LE CRC at payload-end+1, then extra RC. The
  * OnePlus startup uses 0x5a5a/0x5a to detect absent extensions. Requiring
  * all four tail bytes0x5a also verifies the extra final EOM. Do not try
  * to compute an undocumented CRC or ignore an extra RC. */
 for(unsigned i=27;i<31;i++)if(b[i]!=0x5a)return -3;
 /* Same application-mode and minimum write size gate as s3910_probe.
  * No part-number whitelist: vendor part_number is an opaque16-byte field. */
 if(b[3]!=1)return -6;
 unsigned max_write=(unsigned)b[24]|((unsigned)b[25]<<8);
 if(max_write&&max_write<6)return -3;
 return 0;
}
#endif
