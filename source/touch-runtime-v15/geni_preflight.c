/* SPDX-License-Identifier: BSD-2-Clause */
#include "geni_preflight.h"
#include "trace.h"
struct clock {
 uint64_t revision;
 efi_status (*get_id)(struct clock *,const char *,uint64_t *);
 efi_status (*enable)(struct clock *,uint64_t);
 efi_status (*disable)(struct clock *,uint64_t);
 efi_status (*is_enabled)(struct clock *,uint64_t,uint8_t *);
 efi_status (*is_on)(struct clock *,uint64_t,uint8_t *);
};
_Static_assert(offsetof(struct clock,is_enabled)==0x20,"IsEnabled ABI");
_Static_assert(offsetof(struct clock,is_on)==0x28,"IsOn ABI");
static const efi_guid guid={0x241afae6,0x885f,0x4f6c,{0xa7,0xea,0xc2,0x8e,0xab,0x79,0xc3,0xe5}};
static const char *const names[5]={"gcc_qupv3_wrap1_core_clk",
 "gcc_qupv3_wrap1_core_2x_clk","gcc_qupv3_wrap_1_m_ahb_clk",
 "gcc_qupv3_wrap_1_s_ahb_clk","gcc_qupv3_wrap1_s4_clk"};
static struct clock *find(efi_boot_services *bs){struct clock *p=0;
 if(!bs||!bs->locate_protocol||EFI_ERROR(bs->locate_protocol(&guid,0,(void **)&p))||
 !p||p->revision!=0x1000b||!p->get_id||!p->enable||!p->disable||!p->is_enabled||!p->is_on)return 0;
 return p;
}
int geni_preflight_release(efi_boot_services *bs,struct geni_preflight *s)
{
 if(!s)return -5;
 if(!s->votes){s->cleanup_result=0;return 0;}
 struct clock *p=find(bs);if(!p)return s->cleanup_result=-3;
 for(int i=4;i>=0;i--)if(s->votes&(1u<<i)){
  op13_trace(44,(unsigned)i*2);
  s->disable_status[i]=p->disable(p,s->ids[i]);
  op13_trace(44,(unsigned)i*2+1);
  if(!EFI_ERROR(s->disable_status[i]))s->votes&=(uint8_t)~(1u<<i);
 }
 return s->cleanup_result=s->votes?-1:0;
}
int geni_preflight_run(efi_boot_services *bs,int enabled,
 int(*read32)(void *,uint64_t,uint32_t *),void *cookie,struct geni_preflight *s)
{
 if(!s||!read32)return -5;
 if(!enabled)return -3;
 if(s->votes)return -6;
 *s=(struct geni_preflight){0};s->result=-1;
 for(unsigned i=0;i<5;i++)s->id_status[i]=s->enable_status[i]=
  s->on_status[i]=s->disable_status[i]=s->enabled_status[i]=UINT64_MAX;
 struct clock *p=find(bs);if(!p)return s->result=-3;
 /* Resolve IDs first. Enable is NOT reference counted: never touch an
  * already-enabled clock. State-check races require serialized loader use. */
 for(unsigned i=0;i<5;i++){
  op13_trace(40,i*2);
  s->id_status[i]=p->get_id(p,names[i],&s->ids[i]);
  op13_trace(40,i*2+1);
  if(EFI_ERROR(s->id_status[i]))return s->result=-1;
 }
 for(unsigned i=0;i<5;i++){
  op13_trace(41,i*2);
  s->enabled_status[i]=p->is_enabled(p,s->ids[i],&s->initial_enabled[i]);
  op13_trace(41,i*2+1);
  if(EFI_ERROR(s->enabled_status[i]))goto done;
  if(!s->initial_enabled[i]) {
   /* Initial state was disabled under serialized loader ownership. Enable
    * may land then return error: own the rollback BEFORE attempting it. */
   s->votes|=(uint8_t)(1u<<i);
   op13_trace(42,i*2);
   s->enable_status[i]=p->enable(p,s->ids[i]);
   op13_trace(42,i*2+1);
   if(EFI_ERROR(s->enable_status[i]))goto done;
  }
  op13_trace(43,i*2);
  s->on_status[i]=p->is_on(p,s->ids[i],&s->on[i]);
  op13_trace(43,i*2+1);
  if(EFI_ERROR(s->on_status[i])||!s->on[i])goto done;
 }
 s->result=fifo_snapshot_read(1,read32,cookie,&s->snapshot);
 s->reads=s->snapshot.completed;
 if(s->reads==2*FIFO_SNAPSHOT_COUNT){
  s->if_disable=s->snapshot.second[1];s->fw_revision=s->snapshot.second[2];
 }
done:
 if(geni_preflight_release(bs,s))return -1;
 return s->result;
}
