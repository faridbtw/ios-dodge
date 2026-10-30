#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "wifi/lx/src/lx_wpa.h"
int main(void){
 const uint8_t expected[32]={0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef,0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2,0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e};
 uint8_t pmk[32];lx_wpa_pmk((const uint8_t*)"password",8,(const uint8_t*)"IEEE",4,pmk);assert(!memcmp(pmk,expected,32));
 struct{uint8_t before[16],data[LX_WPA_REPLY_MOST],after[16];}reply;
 uint8_t input[300],rsn[20]={0};unsigned seed=7;
 for(unsigned test=0;test<10000;test++){
  struct lx_wpa wpa={0};wpa.rsn=rsn;wpa.rsn_length=sizeof rsn;
  for(unsigned i=0;i<sizeof input;i++){seed=1664525*seed+1013904223;input[i]=seed>>24;}
  memset(&reply,0xa5,sizeof reply);unsigned n=0;const char*why=0;
  (void)lx_wpa_message(&wpa,input,test%301,reply.data,&n,&why);
  for(unsigned i=0;i<16;i++)assert(reply.before[i]==0xa5&&reply.after[i]==0xa5);
 }
 puts("Imported WPA2 logic: standard PBKDF2 vector and10000 malformed/truncated EAPOL messages passed with reply bounds intact");
}
