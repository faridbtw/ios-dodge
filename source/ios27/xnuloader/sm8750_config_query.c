/* SPDX-License-Identifier: BSD-2-Clause
 * Getter ABI transcribed from exact SPI.efi call sites:
 * 4f50..4f80: path(&16-byte node,path), u8(&node,"num_se",&byte)
 * 5374..5380 copies the 16-byte node token to client state
 * 5404..5418: u32(&wrapper-node,"core_base_addr",&uint32)
 * 87dc->23ac dispatch+30; 87f0->241c dispatch+c8;
 * 87ec->2400 dispatch+a0. All return signed32 status (0 success), no This.
 */
#include "sm8750_config_query.h"
struct cfg_node { uint64_t opaque[2]; };
struct cfg_protocol {
 void *prefix[6];
 int32_t (*path)(struct cfg_node *,const char *); /* +0x30 */
 void *middle[13];
 int32_t (*u32)(const struct cfg_node *,const char *,uint32_t *); /* +0xa0 */
 void *gap[4];
 int32_t (*u8)(const struct cfg_node *,const char *,uint8_t *); /* +0xc8 */
};
_Static_assert(offsetof(struct cfg_protocol,path)==0x30,"path ABI");
_Static_assert(offsetof(struct cfg_protocol,u32)==0xa0,"u32 ABI");
_Static_assert(offsetof(struct cfg_protocol,u8)==0xc8,"u8 ABI");
int sm8750_loader_config_query(efi_boot_services *bs,int enabled,
 struct sm8750_config_query_report *r)
{
 static const efi_guid guid={0x0389b776,0x625f,0x11eb,{0x83,0xbe,0xc7,0x41,0xa9,0x13,0xde,0x34}};
 struct cfg_protocol *p=0; struct cfg_node nodes[3]={0};
 static const char *const paths[3]={"/soc/TOP_QUP_0","/soc/TOP_QUP_1","/soc/TOP_QUP_1/TOP_QUP_1_SE_4"};
 if(!bs||!r) return -5;
 if(!enabled) return -3;
 *r=(struct sm8750_config_query_report){0};
 r->locate_status=bs->locate_protocol(&guid,0,(void **)&p);
 if(EFI_ERROR(r->locate_status)||!p||!p->path||!p->u32||!p->u8) return -3;
 for(unsigned i=0;i<3;i++) {
  r->path_status[i]=p->path(&nodes[i],paths[i]);
  if(r->path_status[i]) return -6;
 }
 r->value_status[0]=p->u8(&nodes[0],"num_se",&r->count0);
 r->value_status[1]=p->u8(&nodes[1],"num_se",&r->count1);
 /* wrapper core base is what SPIDxe reads at0x5418; SE stride must NOT be
  * guessed. Ask the SE-specific node too, and only mark exact0xa90000. */
 uint32_t wrapper_base=0,se_base=0;
 r->value_status[2]=p->u32(&nodes[1],"core_base_addr",&wrapper_base);
 r->value_status[3]=p->u32(&nodes[2],"core_base_addr",&se_base);
 r->common_base=wrapper_base; r->core_base=se_base;
 /* Exact consumer: wrapper u32 at5488/5450; child u8 at5568.
  * 6d64..6d74 computes se_wrapper_base + (se_index << 14). */
 uint32_t se_wrapper=0,common=0;
 r->extended_status[0]=p->u32(&nodes[1],"se_wrapper_base_addr",&se_wrapper);
 r->extended_status[1]=p->u8(&nodes[2],"se_index",&r->se_index);
 r->extended_status[2]=p->u32(&nodes[1],"common_base_addr",&common);
 r->se_wrapper_base=se_wrapper; r->wrapper_common_base=common;
 if(!r->extended_status[0]&&!r->extended_status[1]&&
    !r->value_status[0]&&!r->value_status[1]&&r->count0>0&&r->count0<=16&&
    r->count1>4&&r->count1<=16&&r->se_index<r->count1) {
  uint64_t offset=(uint64_t)r->se_index<<14;
  if(r->se_wrapper_base<=UINT64_MAX-offset) {
   r->derived_base=r->se_wrapper_base+offset;
   r->derived_matched_a90000=(r->derived_base==0xa90000);
  }
 }

 if(r->value_status[0]||r->value_status[1]||r->value_status[3]||
    !r->count0||r->count0>16||r->count1<=4||r->count1>16) return -6;
 r->logical_instance=1u+r->count0+4u;
 r->matched_a90000=(r->core_base==0xa90000);
 return r->matched_a90000?0:-6;
}
