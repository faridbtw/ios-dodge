/* SPDX-License-Identifier: BSD-2-Clause
 * Bounded reader for the actual Dodge AW88461 ACF v1/register container.
 * Format/checksums from Awinic's published format; no firmware embedded here.
 * Returns data only after all sections validate. Does not write hardware.
 */
#ifndef OP13_AUDIO_AMP_PROFILE_H
#define OP13_AUDIO_AMP_PROFILE_H
#include <stdint.h>
#include <stddef.h>
struct op13_aw_profile {const uint8_t *registers;unsigned count;};
static uint32_t op13_aw_u32(const uint8_t *b)
{return (uint32_t)b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24;}
static uint16_t op13_aw_u16(const uint8_t *b)
{return (uint16_t)(b[0]|b[1]<<8);}
static uint8_t op13_aw_crc(const uint8_t *b,unsigned bytes)
{
 uint8_t crc=0;
 for(unsigned i=0;i<bytes;i++){
  crc^=b[i];for(unsigned bit=0;bit<8;bit++)crc=(crc>>1)^((crc&1)?0x8c:0);
 }
 return crc;
}
static int op13_aw_music_profile(const uint8_t *b,size_t bytes,unsigned index,
 struct op13_aw_profile *out)
{
 if(!out)return -1;
 out->registers=0;out->count=0;
 if(!b||bytes<80||bytes>16384||index>1||op13_aw_u32(b)!=0x0a15f908||
    op13_aw_u32(b+64)!=1)return -1;
 uint32_t count=op13_aw_u32(b+56),table=op13_aw_u32(b+60),size=op13_aw_u32(b+52);
 if(!count||count>16||table<80||table>bytes||size!=count*64||size>bytes-table)return -1;
 const uint8_t *selected=0;unsigned selected_count=0;
 for(unsigned i=0;i<count;i++){
  const uint8_t *entry=b+table+64*i;
  uint32_t n=op13_aw_u32(entry+32),at=op13_aw_u32(entry+36);
  if(op13_aw_u32(entry)!=2||op13_aw_u32(entry+28)!=4||n<64||
     at<table+size||at>bytes||n>bytes-at||
     op13_aw_u32(entry+40)!=op13_aw_crc(b+at,n))return -2;
  const uint8_t *bin=b+at;
  if(op13_aw_u32(bin+4)!=0x01000000||op13_aw_u32(bin+8)!=0||
     op13_aw_u32(bin+12)!=1||op13_aw_u32(bin+16)!=n-60||
     op13_aw_u32(bin+32)!=2||op13_aw_u32(bin+36)!=2)return -3;
  const char chip[8]="AW88461";
  for(unsigned c=0;c<8;c++)if(bin[24+c]!=(uint8_t)chip[c])return -3;
  uint32_t sum=0;for(unsigned c=4;c<n;c++)sum+=bin[c];
  unsigned regs=op13_aw_u32(bin+60);
  if(sum!=op13_aw_u32(bin)||!regs||regs>128||n!=64+regs*4)return -3;
  /* Validate every native destination before returning any selected profile.
   * The ACF terminates with ff/0; it is a marker, not a register we enable.
   */
  for(unsigned r=0;r<regs;r++){
   unsigned reg=op13_aw_u16(bin+64+4*r),value=op13_aw_u16(bin+66+4*r);
   if(!((reg>=3&&reg<=0x1f)||(reg>=0x30&&reg<=0x3f&&reg!=0x39)||
        (reg>=0x50&&reg<=0x6d)||(reg==0xff&&!value&&r==regs-1)))return -4;
  }
  if(op13_aw_u16(entry+20)==index&&op13_aw_u16(entry+26)==0){
   if(selected)return -5;
   selected=bin+64;selected_count=regs;
  }
 }
 if(!selected)return -6;
 out->registers=selected;out->count=selected_count;return 0;
}
#endif
