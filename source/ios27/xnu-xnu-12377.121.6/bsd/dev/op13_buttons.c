/* SPDX-License-Identifier: BSD-2-Clause
 * New implementation from public Linux register interfaces. See REFERENCES.md.
 */
#ifdef KERNEL
#include <sys/op13_buttons.h>
#else
#include "op13_buttons.h"
#endif
static void clear(void *p,size_t n) { uint8_t *b=p; while(n--) *b++=0; }
static void note(struct op13_buttons *d,const char *s,uint32_t a,uint32_t b)
{ if(d->host.diagnostic) d->host.diagnostic(d->host.cookie,s,a,b); }
static uint32_t rd(struct op13_buttons *d,enum op13_region r,uint32_t o)
{ return d->host.read32(d->host.cookie,r,o); }

int op13_version(struct op13_buttons *d,const struct op13_host *h)
{
 if(!d||!h||!h->read32||!h->issue_read||!h->lock||!h->unlock||
    !h->delay_us||!h->time_us||!h->event) return OP13_RANGE;
 clear(d,sizeof(*d)); d->host=*h;
 d->version=rd(d,OP13_CORE,0);
 note(d,"arbiter version",d->version,0);
 /* v8 uses separate chnl_map/chnl_owner resources absent from this DT.
  * A v8 value must not be interpreted using the v7 map or owner locations. */
 if(d->version<0x70000000u||d->version>=0x80000000u) return OP13_UNSUPPORTED;
 /* Actual DT: bus0, EE0. Only primary-bus count applies. */
 d->count=rd(d,OP13_CORE,4)&0x7ffu;
 if(!d->count||d->count>1024||0x2000+4*d->count>OP13_CORE_SIZE||
    4*d->count>OP13_CNFG_SIZE) return OP13_RANGE;
 note(d,"bus0 APID count / EE",d->count,0);
 d->stage=1; return 0;
}

int op13_find_apids(struct op13_buttons *d)
{
 const unsigned wanted[2]={0x013,0x18d};
 unsigned found[2]={0,0};
 if(d->stage!=1||d->poisoned) return OP13_NOT_READY;
 for(unsigned i=0;i<d->count;i++) {
  uint32_t map=rd(d,OP13_CORE,0x2000+4*i);
  if(!map) continue;
  unsigned ppid=(map>>8)&0xfffu;
  for(unsigned k=0;k<2;k++) if(ppid==wanted[k]) {
   unsigned owner=rd(d,OP13_CNFG,4*i)&7;
   /* Linux v7: first mapping, overridden by each mapping owned by our EE. */
   if(!found[k]||owner==0) {
    found[k]=1; d->apid[k]=(uint16_t)i; d->owner[k]=(uint8_t)owner;
    d->observer[k]=0x20*i; /* observer EE stride0x8000, EE0 */
   }
  }
 }
 for(unsigned k=0;k<2;k++) {
  if(!found[k]||d->observer[k]+0x20>OP13_OBSERVER_SIZE) {
   note(d,"button PPID missing",wanted[k],found[k]); return OP13_NOT_READY;
  }
  note(d,k?"GPIO6 APID / write owner":"PON APID / write owner",d->apid[k],d->owner[k]);
 }
 /* Observer reads do not require write ownership (Linux pmic_arb_offset_v7).
  * Actual per-transaction permission denial is checked, never bypassed. */
 d->stage=2; return 0;
}

int op13_read(struct op13_buttons *d,unsigned device,unsigned offset,uint8_t *out,size_t n)
{
 uint32_t status=0,words[2]={0,0};
 unsigned channel;
 int error=OP13_TIMEOUT;
 if(d->stage<2||d->poisoned) return OP13_NOT_READY;
 if(!out||device>1||!n||n>8||offset>255||n>256-offset) return OP13_RANGE;
 /* Do not expose arbitrary PMIC registers even through read-only commands. */
 if(!((offset==4&&n==2)||(offset==0x10&&n==1)||
      (device==1&&offset==0x40&&n==8))) return OP13_RANGE;
 channel=d->observer[device];
 if(channel>OP13_OBSERVER_SIZE-0x20) return OP13_RANGE;
 d->host.lock(d->host.cookie);
 /* EXT_READL opcode1, low address byte, byte count minus one. SID/peripheral
  * are selected by APID; no raw SID bits in v7 command format. */
 d->host.issue_read(d->host.cookie,channel,(1u<<27)|(offset<<4)|(uint32_t)(n-1));
 d->transactions++;
 for(unsigned i=0;i<1000;i++) {
  status=rd(d,OP13_OBSERVER,channel+8);
  if(status&1) {
   if(status&4) error=OP13_DENIED;
   else if(status&0xau) error=OP13_IO;
   else {
    words[0]=rd(d,OP13_OBSERVER,channel+0x18);
    if(n>4) words[1]=rd(d,OP13_OBSERVER,channel+0x1c);
    error=0;
   }
   break;
  }
  d->host.delay_us(d->host.cookie,1);
 }
 d->host.unlock(d->host.cookie);
 d->last_status=status;
 if(error) {
  d->poisoned=1;
  note(d,"SPMI read failed: device/offset, status",(device<<16)|offset,status);
  return error;
 }
 for(size_t i=0;i<n;i++) out[i]=(uint8_t)(words[i/4]>>(8*(i%4)));
 return 0;
}

int op13_identify(struct op13_buttons *d)
{
 uint8_t type[2]; int e;
 if(d->stage!=2||d->poisoned) return OP13_NOT_READY;
 e=op13_read(d,0,4,type,2); if(e) return e;
 note(d,"PON type/subtype",type[0],type[1]);
 if(type[0]!=1||type[1]!=9) return OP13_UNSUPPORTED;
 e=op13_read(d,1,4,type,2); if(e) return e;
 note(d,"GPIO6 type/subtype",type[0],type[1]);
 if(type[0]!=0x10||type[1]<0x10||type[1]>0x15) return OP13_UNSUPPORTED;
 d->stage=3; return 0;
}

int op13_validate_inputs(struct op13_buttons *d)
{
 uint8_t cfg[8],pon,gpio; int e;
 if(d->stage!=3||d->poisoned) return OP13_NOT_READY;
 e=op13_read(d,1,0x40,cfg,8); if(e) return e;
 note(d,"GPIO6 mode/pull, VIN/enable",((cfg[0]&3)<<8)|(cfg[2]&7),
      ((cfg[1]&7)<<8)|cfg[6]);
 /* Normal digital input with a pull-up, enabled; never correct a pin by write. */
 if((cfg[0]&3)!=0||(cfg[2]&7)>=4||!(cfg[6]&0x80)) return OP13_NOT_READY;
 e=op13_read(d,0,0x10,&pon,1); if(e) return e;
 e=op13_read(d,1,0x10,&gpio,1); if(e) return e;
 note(d,"initial PON / GPIO6 state",pon,gpio);
 clear(d->keys,sizeof(d->keys));
 d->stage=4; return 0;
}

static void sample(struct op13_buttons *d,unsigned key,unsigned pressed,uint64_t now)
{
 struct op13_key_state *s=&d->keys[key];
 pressed=!!pressed;
 if(!s->sampled||s->candidate!=pressed) {
  s->sampled=1; s->candidate=(uint8_t)pressed; s->candidate_since=now; return;
 }
 if(now-s->candidate_since<OP13_DEBOUNCE_US) return;
 /* Suppress a key held at boot until first stable release. */
 if(!s->armed) { if(!pressed) s->armed=1; return; }
 if(s->stable==pressed) return;
 s->stable=(uint8_t)pressed;
 d->host.event(d->host.cookie,key,pressed);
}

void op13_stop(struct op13_buttons *d)
{
 if(!d) return;
 for(unsigned k=0;k<3;k++) {
  if(d->keys[k].stable&&d->host.event) d->host.event(d->host.cookie,k,0);
  clear(&d->keys[k],sizeof(d->keys[k]));
 }
 d->stage=0;
}

int op13_poll(struct op13_buttons *d)
{
 uint8_t pon,gpio; uint64_t now; int e;
 if(d->stage!=4||d->poisoned) return OP13_NOT_READY;
 e=op13_read(d,0,0x10,&pon,1); if(e) goto stop;
 e=op13_read(d,1,0x10,&gpio,1); if(e) goto stop;
 now=d->host.time_us(d->host.cookie);
 sample(d,OP13_POWER,pon&0x80,now);
 sample(d,OP13_VOLUP,pon&0x40,now);
 sample(d,OP13_VOLDOWN,!(gpio&1),now);
 return 0;
stop:
 op13_stop(d); return e;
}
