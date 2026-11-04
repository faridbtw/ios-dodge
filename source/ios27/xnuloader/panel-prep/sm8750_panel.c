#include "trace.h"
/* SPDX-License-Identifier: BSD-2-Clause */
#include "sm8750_panel.h"
static uint32_t offset(unsigned pin, unsigned reg) { return (161+pin)*0x1000u+reg; }
static int get(struct sm8750_panel *s,unsigned p,unsigned r,uint32_t *v)
{ return s->io.read(s->io.cookie,offset(p,r),v); }
static int set(struct sm8750_panel *s,unsigned p,unsigned r,uint32_t v,uint32_t mask)
{
 uint32_t got;
 if(s->io.write(s->io.cookie,offset(p,r),v)||get(s,p,r,&got)) return -1;
 return ((got^v)&mask)?-1:0;
}
static int clients(struct sm8750_panel *s)
{ return s->power.npa_clients[0]||s->power.npa_clients[1]||s->power.npa_clients[2]; }
int sm8750_panel_release(struct sm8750_panel *s)
{
 int failed=0;
 if(!s) return -5;
 s->prepared=0;
 if(s->avdd.owned||clients(s))s->power_settle_pending=1;
 if(s->pins_owned) {
  /* Hold reset low until our own supply changes are undone. */
  uint32_t cfg, latch;
  if(get(s,0,0,&cfg)||get(s,0,4,&latch)||
     set(s,0,4,latch&~2u,2)||
     set(s,0,0,(cfg&~0x3ffu)|0x2c0u,0x3ff)) failed=1;
 }
 if(!failed) {
  if(sm8750_loader_avdd_restore(s->bs,&s->avdd)) failed=1;
  if(!s->avdd.owned) sm8750_loader_power_release(s->bs,&s->power);
  if(s->avdd.owned||clients(s)) failed=1;
 }
 /* Match power_on(false)'s200ms while reset remains low. Persist across
  * failed waits and retry before restoring pins, even after votes are gone. */
 if(!failed&&s->power_settle_pending) {
  if(!s->bs||!s->bs->stall||EFI_ERROR(s->bs->stall(200000)))failed=1;
  else s->power_settle_pending=0;
 }
 /* Destroying our NPA clients does not establish physical VDD-off. */
 if(!failed&&s->pins_owned) {
  uint32_t cfg;
  if(get(s,0,0,&cfg)||set(s,0,0,cfg&~0x200u,0x3ff)||
     set(s,0,4,s->saved_io[0],2)||set(s,0,0,s->saved_cfg[0],0x3ff)||
     set(s,1,0,s->saved_cfg[1],0x3ff)) failed=1;
  if(!failed)s->pins_owned=0;
 }
 if(s->pins_owned||s->avdd.owned||clients(s)||s->power_settle_pending) failed=1;
 s->cleanup_error=failed?-1:0;
 return s->cleanup_error;
}
int sm8750_panel_prepare(struct sm8750_panel *s,efi_boot_services *bs,
 const struct sm8750_panel_io *io,int enabled)
{
 int e;
 if(!s||!bs||!io) return -5;
 if(!enabled) return -3;
 if(s->prepared||s->pins_owned||s->avdd.owned||clients(s)||s->power_settle_pending) return -6;
 if(!io->read||!io->write||!bs->stall) return -3;
 s->bs=bs; s->io=*io; s->error=0; s->cleanup_error=0; s->stage=1;
 for(unsigned i=0;i<2;i++) {
  op13_trace(27,2*i);
  if(get(s,i,0,&s->saved_cfg[i])||get(s,i,4,&s->saved_io[i])) return s->error=-1;
  op13_trace(27,2*i+1);
  /* Reject eGPIO ownership and existing alternate functions. */
  if(s->saved_cfg[i]&(0x1800u|0x3cu)) return s->error=-3;
 }
 if(s->saved_io[0]&2u) return s->error=-3; /* refuse inherited reset-high */
 /* Establish captured pinctrl reset-low before either supply operation.
  * Claim snapshots BEFORE first write: even a failed write may have landed. */
 e=-1;s->stage=2;
 s->pins_owned=1;
 /* Preload low before output-enable. Preserve non-config bits. 8mA/no pull. */
 if(set(s,0,4,s->saved_io[0]&~2u,2)||
    set(s,0,0,(s->saved_cfg[0]&~0x3ffu)|0x2c0u,0x3ff)||
    set(s,1,0,(s->saved_cfg[1]&~0x3ffu)|0xc0u,0x3ff)) goto fail;
 s->stage=3;op13_trace(23,0);
 e=sm8750_loader_avdd_enable(bs,1,&s->avdd);
 if(s->avdd.stage>=2||s->avdd.owned)s->power_settle_pending=1;
 if(e) goto fail;
 s->stage=4;op13_trace(22,0);
 e=sm8750_loader_power_vote(bs,1,&s->power); if(e) goto fail;
 op13_trace(24,0);e=-1;
 if(EFI_ERROR(bs->stall(200000))) goto fail;
 s->stage=5;op13_trace(25,0);
 if(EFI_ERROR(bs->stall(10000))||set(s,0,4,s->saved_io[0]|2,2)||
    EFI_ERROR(bs->stall(80000))) goto fail;
 s->stage=6;op13_trace(26,0);
 for(unsigned i=0;i<2;i++) if(get(s,i,0,&s->observed_cfg[i])||
    get(s,i,4,&s->observed_io[i])) goto fail;
 if((s->observed_cfg[0]&0x3ff)!=0x2c0||
    (s->observed_cfg[1]&0x3ff)!=0xc0||!(s->observed_io[0]&2)) goto fail;
 s->prepared=1; s->stage=7; return 0;
fail:
 s->error=e; (void)sm8750_panel_release(s); return e;
}
