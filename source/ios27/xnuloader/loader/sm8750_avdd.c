#include "trace.h"
/* SPDX-License-Identifier: BSD-2-Clause */
#include "sm8750_avdd.h"
struct avdd_spmi;
struct avdd_spmi {
 uint64_t revision;
 uint32_t (*read)(struct avdd_spmi *,uint32_t,uint32_t,uint32_t,uint32_t,uint8_t *,uint32_t,uint32_t *);
 uint32_t (*write)(struct avdd_spmi *,uint32_t,uint32_t,uint32_t,uint32_t,const uint8_t *,uint32_t);
};
static const efi_guid guid={0xfa5f306b,0xf47d,0x4ac4,{0xa4,0x7d,0x88,0x2f,0x82,0x04,0xec,0x30}};
static struct avdd_spmi *find(efi_boot_services *bs)
{
 struct avdd_spmi *p=0;
 if(!bs||EFI_ERROR(bs->locate_protocol(&guid,0,(void **)&p))||!p||
    p->revision!=0x10003||!p->read||!p->write) return 0;
 return p;
}
static int read_at(struct avdd_spmi *p,unsigned a,uint8_t *v,unsigned n)
{
 uint32_t done=0;
 return p->read(p,0,9,0,a,v,n,&done)||done!=n?-1:0;
}
static int write_at(struct avdd_spmi *p,unsigned a,uint8_t value)
{
 uint8_t got=0;
 op13_trace(20,a);
 if(p->write(p,0,9,0,a,&value,1)||read_at(p,a,&got,1)||got!=value) return -1;
 op13_trace(21,a);return 0;
}
int sm8750_loader_avdd_restore(efi_boot_services *bs,struct sm8750_avdd_session *s)
{
 struct avdd_spmi *p;
 if(!s) return -5;
 if(!s->owned) return 0;
 p=find(bs); if(!p) return -3;
 /* Disable while restoring source/direction, then original enable. */
 if(write_at(p,0x8a46,s->configured[6]&0x7f)||
    write_at(p,0x8a44,s->original[4])||write_at(p,0x8a40,s->original[0])||
    write_at(p,0x8a46,s->original[6])) return -1;
 s->owned=0; return 0;
}
int sm8750_loader_avdd_enable(efi_boot_services *bs,int enabled,struct sm8750_avdd_session *s)
{
 struct avdd_spmi *p; uint8_t id[2];
 if(!bs||!s) return -5;
 if(!enabled) return -3;
 if(s->owned) return -6;
 p=find(bs); if(!p) return -3;
 s->stage=1;
 if(read_at(p,0x8a04,id,2)||id[0]!=0x10||id[1]<0x10||id[1]>0x15) return -3;
 s->subtype=id[1];
 if(read_at(p,0x8a40,s->original,7)) return -1;
 /* GPIO debugfs capture: VIN1, pull-down, CMOS, low strength. */
 if((s->original[1]&7)!=1||(s->original[2]&7)!=4||
    (s->original[5]&0x33)!=1) return -3;
 /* Restore only the observed input/normal-low inherited AVDD state. */
 if((s->original[0]&3u)!=0||(s->original[4]&0x8fu)!=0) return -3;
 for(unsigned i=0;i<7;i++) s->configured[i]=s->original[i];
 s->configured[0]=(s->original[0]&~3u)|1; /* LV/MV digital output */
 s->configured[4]=(s->original[4]&~0x8fu)|0x80; /* normal, high */
 s->configured[6]=s->original[6]|0x80;
 /* Mark ownership before the first write so partial failures can restore. */
 s->owned=1; s->stage=2;
 if(write_at(p,0x8a44,s->configured[4])||write_at(p,0x8a40,s->configured[0])||
    write_at(p,0x8a46,s->configured[6])) {
  s->result=1; (void)sm8750_loader_avdd_restore(bs,s); return -1;
 }
 s->stage=3; s->result=0; return 0;
}
