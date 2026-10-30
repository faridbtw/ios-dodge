/* SPDX-License-Identifier: BSD-2-Clause
 * Fixed live Dodge FDT supplies; resource names come from qcom,resource-name.
 * Reserve every address/limit before issuing any RPMh vote. */
#ifndef OP13_WIFI_RAILS_H
#define OP13_WIFI_RAILS_H
struct op13_wifi_rail {const char *name;unsigned min_mv,max_mv;};
static const struct op13_wifi_rail op13_wifi_rails[]={
 {"ldof3",1800,1800},{"ldof2",1200,1200},{"smpd4",876,1036},
 {"smpf5",876,1000},{"smpg3",1860,2000},{"smpi7",1312,1340}
};
static int op13_wifi_rail_plan(const uint32_t address[6],uint32_t voltage[6])
{
 if(!address||!voltage)return -1;
 uint32_t v[6];
 for(unsigned i=0;i<6;i++){
  if(!address[i]||(address[i]&15)||((address[i]>>16)&7)!=4)return -2;
  for(unsigned j=0;j<i;j++)if(address[j]==address[i])return -3;
  /* SMPS rounding to8mV fits both4mV and8mV native selectors. */
  v[i]=i<2?op13_wifi_rails[i].min_mv:(op13_wifi_rails[i].min_mv+7)&~7u;
  if(v[i]<op13_wifi_rails[i].min_mv||v[i]>op13_wifi_rails[i].max_mv)return -4;
 }
 for(unsigned i=0;i<6;i++)voltage[i]=v[i];return 0;
}
#endif
