/* SPDX-License-Identifier: BSD-2-Clause
 * ABI checked against exact Mu-dodge-v3.1-BootPrep-v0.3.img extraction.
 * See reference/uefi-v3.1/{manifest.json,SPMI.disasm.txt,ClockDxe-prefix.disasm.txt}.
 * No EFI protocol calls may be made from XNU through these pointers.
 */
#include "sm8750_prepare.h"

struct clock_protocol;
struct clock_protocol {
 uint64_t revision;
 efi_status (*get_id)(struct clock_protocol *,const char *,uint64_t *);
 efi_status (*enable)(struct clock_protocol *,uint64_t);
 efi_status (*disable)(struct clock_protocol *,uint64_t);
 efi_status (*is_enabled)(struct clock_protocol *,uint64_t,uint8_t *);
 efi_status (*is_on)(struct clock_protocol *,uint64_t,uint8_t *);
 efi_status (*set_frequency)(struct clock_protocol *,uint64_t,uint32_t,unsigned,uint32_t *);
 efi_status (*get_frequency)(struct clock_protocol *,uint64_t,uint32_t *);
};
struct spmi_protocol;
struct spmi_protocol {
 uint64_t revision;
 /* v1.3: This, bus, slave, priority, address, buffer, size, bytes_read.
  * v1.0's seven-argument declaration is incompatible with this eight-argument
  * entry. In the extracted binary x7 is the returned-byte-count pointer;
  * internal 0x2730 bounds the first scalar against the bus count. */
 uint32_t (*read_long)(struct spmi_protocol *,uint32_t,uint32_t,uint32_t,
                       uint32_t,uint8_t *,uint32_t,uint32_t *);
};
static const efi_guid clock_guid={0x241afae6,0x885f,0x4f6c,{0xa7,0xea,0xc2,0x8e,0xab,0x79,0xc3,0xe5}};
static const efi_guid spmi_guid={0xfa5f306b,0xf47d,0x4ac4,{0xa4,0x7d,0x88,0x2f,0x82,0x04,0xec,0x30}};
static const char *const clocks[3]={"gcc_qupv3_wrap_1_m_ahb_clk",
 "gcc_qupv3_wrap_1_s_ahb_clk","gcc_qupv3_wrap1_s4_clk"};

void sm8750_loader_release_clocks(efi_boot_services *bs,struct sm8750_prepare_report *r)
{
 struct clock_protocol *p=0;
 if(!bs||!r||!r->clock_votes) return;
 if(EFI_ERROR(bs->locate_protocol(&clock_guid,0,(void **)&p))||!p||p->revision!=0x1000b||!p->disable) return;
 for(int i=2;i>=0;i--) if(r->clock_votes&(1u<<i)) {
  efi_status s=p->disable(p,r->clock_id[i]);
  if(!EFI_ERROR(s)) r->clock_votes&=(uint8_t)~(1u<<i);
  else r->clock_status[i]=s;
 }
}
int sm8750_loader_prepare(efi_boot_services *bs,int enabled,int vote,
                          struct sm8750_prepare_report *r)
{
 struct clock_protocol *c=0; struct spmi_protocol *s=0;
 if(!bs||!r) return -5;
 if(!enabled) return -3;
 /* Do not discard outstanding enable votes if called twice. */
 if(r->clock_votes) return -6;
 *r=(struct sm8750_prepare_report){0};
 r->spmi_result=UINT32_MAX; /* protocol call not attempted */
 for(unsigned i=0;i<3;i++) {
  r->clock_status[i]=r->clock_id_status[i]=r->clock_frequency_status[i]=
   r->clock_on_status[i]=UINT64_MAX;
 }
 r->clock_locate_status=bs->locate_protocol(&clock_guid,0,(void **)&c);
 r->spmi_locate_status=bs->locate_protocol(&spmi_guid,0,(void **)&s);
 if(!EFI_ERROR(r->clock_locate_status)&&c) r->clock_revision=c->revision;
 if(!EFI_ERROR(r->spmi_locate_status)&&s) r->spmi_revision=s->revision;
 /* v2 read-only path: independently sample supported exact revisions.
  * UINT64_MAX means not called; never invoke rate/state with failed ID. */
 if(!vote) {
  int failed=0;
  for(unsigned i=0;i<3;i++) {
   r->clock_id_status[i]=r->clock_frequency_status[i]=r->clock_on_status[i]=UINT64_MAX;
   if(r->clock_revision!=0x1000b||!c||!c->get_id||!c->get_frequency||!c->is_on) {failed=1;continue;}
   r->clock_id_status[i]=c->get_id(c,clocks[i],&r->clock_id[i]);
   if(EFI_ERROR(r->clock_id_status[i])) {failed=1;continue;}
   r->clock_frequency_status[i]=c->get_frequency(c,r->clock_id[i],&r->frequency[i]);
   r->clock_on_status[i]=c->is_on(c,r->clock_id[i],&r->clock_on[i]);
   if(EFI_ERROR(r->clock_frequency_status[i])||EFI_ERROR(r->clock_on_status[i])) failed=1;
  }
  if(r->spmi_revision!=0x10003||!s||!s->read_long) return -3;
  r->spmi_result=s->read_long(s,0,9,0,0x8a04,r->avdd_type_subtype,2,&r->spmi_bytes);
  if(r->spmi_result||r->spmi_bytes!=2||r->avdd_type_subtype[0]!=0x10) return -1;
  r->spmi_result=s->read_long(s,0,9,0,0x8a40,r->avdd_control,8,&r->spmi_bytes);
  return (failed||r->spmi_result||r->spmi_bytes!=8)?-1:0;
 }
 /* Legacy vote path remains exact-revision gated; diagnostics never request it. */
 if(r->clock_revision!=0x1000b||r->spmi_revision!=0x10003||
    !c->get_id||!c->get_frequency||!c->is_on||!c->enable||!c->disable||!s->read_long) return -3;
 for(unsigned i=0;i<3;i++) {
  r->clock_status[i]=c->get_id(c,clocks[i],&r->clock_id[i]);
  if(EFI_ERROR(r->clock_status[i])) goto clock_fail;
  if(vote) {
   r->clock_status[i]=c->enable(c,r->clock_id[i]);
   if(EFI_ERROR(r->clock_status[i])) goto clock_fail;
   r->clock_votes|=(uint8_t)(1u<<i);
  }
  r->clock_status[i]=c->get_frequency(c,r->clock_id[i],&r->frequency[i]);
  if(EFI_ERROR(r->clock_status[i])) goto clock_fail;
  r->clock_status[i]=c->is_on(c,r->clock_id[i],&r->clock_on[i]);
  if(EFI_ERROR(r->clock_status[i])) goto clock_fail;
 }
 /* AP SPMI bus0 from captured DT. ReadLong priority0 is low priority.
  * Only this fixed allowlist is read. No generic address/write entry exposed. */
 r->spmi_result=s->read_long(s,0,9,0,0x8a04,r->avdd_type_subtype,2,&r->spmi_bytes);
 if(r->spmi_result||r->spmi_bytes!=2) goto clock_fail;
 if(r->avdd_type_subtype[0]!=0x10) goto clock_fail;
 r->spmi_result=s->read_long(s,0,9,0,0x8a40,r->avdd_control,8,&r->spmi_bytes);
 if(r->spmi_result||r->spmi_bytes!=8) goto clock_fail;
 return 0;
clock_fail:
 sm8750_loader_release_clocks(bs,r);
 return -1;
}
