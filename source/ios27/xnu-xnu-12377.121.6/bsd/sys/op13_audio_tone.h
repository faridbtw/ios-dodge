/* SPDX-License-Identifier: BSD-2-Clause
 * Finite500Hz diagnostic tone:100ms, peak8192/32768,5ms fades, stereo16.
 * Hardware adds at least36dB attenuation. Everything after100ms stays zero.
 */
#ifndef OP13_AUDIO_TONE_H
#define OP13_AUDIO_TONE_H
#include <stdint.h>
#include <stddef.h>
static int op13_audio_quiet_tone(int16_t *out,size_t bytes)
{
 if(!out||bytes<32768||(bytes&1))return -1;
 for(size_t i=0;i<bytes/2;i++)out[i]=0;
 for(unsigned frame=0;frame<4800;frame++){
  unsigned p=frame%96;int32_t v=((int32_t)(p<48?p:96-p)-24)*8192/24;
  unsigned fade=frame<4799-frame?frame:4799-frame;if(fade>240)fade=240;
  v=v*(int32_t)fade/240;out[2*frame]=out[2*frame+1]=(int16_t)v;
 }
 return 0;
}
#endif
