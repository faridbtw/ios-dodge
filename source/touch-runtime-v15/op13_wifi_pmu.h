/* SPDX-License-Identifier: BSD-2-Clause
 * Native CNSS OL-CPR wire format and Dodge DT mapping, independently encoded.
 * Plan every rail before writes. Firmware input never selects arbitrary rails.
 */
#ifndef OP13_WIFI_PMU_H
#define OP13_WIFI_PMU_H
#include "op13_wifi_qmi.h"
static const char *const op13_pmu_pins[]={
 "VDDD_AON_0P9","VDDA_RFA_1P9","VDDA_RFA_1P3","VDDA_RFA_0P9",
 "VDDD_WLMX_0P9","VDDD_WLCX_0P9","VDDD_BTCX_0P9","VDDD_BTCMX_0P9",
 "VDDA_PCIE_1P2","VDDA_PCIE_0P9"
};
static const unsigned op13_pmu_rail[]={0,2,3,0,1,0,0,0,3,3};
static const char *const op13_pmu_rails[]={"s5f","s4d","s3g","s7i"};
/* Actual phone DT regulator limits, mV. Conservative for sleep as well. */
static const unsigned op13_pmu_min[]={500,500,1856,1224};
static const unsigned op13_pmu_max[]={1000,1036,2000,1340};
struct op13_pmu {
 unsigned count;
 struct {unsigned pin,wake_valid,wake,sleep_valid,sleep;} item[16];
};
struct op13_pmu_plan {unsigned mv[4][2];};
static int op13_pmu_decode(const uint8_t *v,size_t n,struct op13_pmu *out)
{
 struct op13_pmu p={0};unsigned seen=0;
 if(!n||!v[0]||v[0]>16||n!=1+48u*v[0])return -1;
 p.count=v[0];
 for(unsigned i=0;i<p.count;i++){
  const uint8_t *s=v+1+48*i;unsigned pin;
  for(pin=0;pin<10;pin++){
   unsigned j=0;while(j<32&&op13_pmu_pins[pin][j]&&s[j]==(uint8_t)op13_pmu_pins[pin][j])j++;
   if(j<32&&!op13_pmu_pins[pin][j]&&!s[j])break;
  }
  if(pin==10||(seen&(1u<<pin)))return -1;seen|=1u<<pin;
  p.item[i].pin=pin;p.item[i].wake_valid=op13_qrtr_u32(s+32);p.item[i].wake=op13_qrtr_u32(s+36);
  p.item[i].sleep_valid=op13_qrtr_u32(s+40);p.item[i].sleep=op13_qrtr_u32(s+44);
  if(p.item[i].wake_valid>1||p.item[i].sleep_valid>1)return -1;
 }
 *out=p;return 0;
}
static int op13_pmu_plan(const struct op13_pmu *p,struct op13_pmu_plan *out)
{
 struct op13_pmu_plan plan={0};
 if(!p->count||p->count>16)return -1;
 for(unsigned i=0;i<p->count;i++){
  if(p->item[i].pin>=10||p->item[i].wake_valid>1||p->item[i].sleep_valid>1)return -1;
  unsigned rail=op13_pmu_rail[p->item[i].pin];
  for(unsigned sleep=0;sleep<2;sleep++){
   if(!(sleep?p->item[i].sleep_valid:p->item[i].wake_valid))continue;
   unsigned raw=sleep?p->item[i].sleep:p->item[i].wake;
   if(raw<100||raw>2000)return -1;
   /* Native roundup(voltage,4) -16 + (wake?30:10), then maximum per rail. */
   unsigned mv=((raw+3)&~3u)-16+(sleep?10:30);
   if(mv>plan.mv[rail][sleep])plan.mv[rail][sleep]=mv;
  }
 }
 for(unsigned r=0;r<4;r++)for(unsigned s=0;s<2;s++)
  if(plan.mv[r][s]&&(plan.mv[r][s]<op13_pmu_min[r]||plan.mv[r][s]>op13_pmu_max[r]))return -2;
 *out=plan;return 0;
}
static int op13_pmu_message(char out[64],unsigned rail,unsigned sleep,unsigned mv)
{
 if(rail>=4||sleep>1||mv<op13_pmu_min[rail]||mv>op13_pmu_max[rail])return -1;
 const char *parts[]={"{class: wlan_pdc, ss: ",rail==1?"bb":"rf",", res: ",op13_pmu_rails[rail],".v, ",sleep?"dwnval":"upval",": "};
 unsigned at=0;for(unsigned i=0;i<7;i++)for(unsigned j=0;parts[i][j];j++){if(at>=55)return -1;out[at++]=parts[i][j];}
 char digits[4];unsigned count=0;do{digits[count++]='0'+mv%10;mv/=10;}while(mv&&count<4);
 while(count)out[at++]=digits[--count];out[at++]='}';out[at]=0;return 0;
}
#endif
