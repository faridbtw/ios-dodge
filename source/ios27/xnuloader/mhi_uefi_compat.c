/* SPDX-License-Identifier: BSD-2-Clause
 * Bounded formatter for the MHI host's integer-only diagnostic formats.
 */
#include "mhi_uefi_compat.h"
static void add(char *out,size_t cap,size_t *n,char c)
{if(*n+1<cap)out[*n]=c;(*n)++;}
int op13_mhi_vsnprintf(char *out,size_t cap,const char *fmt,va_list ap)
{
 size_t n=0;
 for(;*fmt;fmt++){
  if(*fmt!='%'){add(out,cap,&n,*fmt);continue;}
  fmt++;int hex_prefix=0,length=0;
  if(*fmt=='#'){hex_prefix=1;fmt++;}
  if(*fmt=='z'){length=3;fmt++;}
  else if(*fmt=='l'){length=1;fmt++;if(*fmt=='l'){length=2;fmt++;}}
  if(*fmt=='%'){add(out,cap,&n,'%');continue;}
  if(*fmt!='x'&&*fmt!='u'&&*fmt!='d'){add(out,cap,&n,'?');continue;}
  uint64_t v;int neg=0;
  if(*fmt=='d'){
   int64_t signed_value=length==2?va_arg(ap,long long):length==1?va_arg(ap,long):va_arg(ap,int);
   neg=signed_value<0;v=neg?0-(uint64_t)signed_value:(uint64_t)signed_value;
  }else v=length==3?va_arg(ap,size_t):length==2?va_arg(ap,unsigned long long):length==1?va_arg(ap,unsigned long):va_arg(ap,unsigned int);
  unsigned base=*fmt=='x'?16:10;char tmp[32];unsigned count=0;
  if(neg)add(out,cap,&n,'-');
  if(hex_prefix&&v){add(out,cap,&n,'0');add(out,cap,&n,'x');}
  do{tmp[count++]="0123456789abcdef"[v%base];v/=base;}while(v);
  while(count)add(out,cap,&n,tmp[--count]);
 }
 if(cap)out[n<cap?n:cap-1]=0;
 return (int)n;
}
