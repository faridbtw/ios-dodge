#include "geni_preflight.h"
static struct geni_preflight geni_state;
static int geni_read_allowlist(void *cookie,uint64_t address,uint32_t *value)
{
 (void)cookie;
 unsigned index=FIFO_SNAPSHOT_COUNT;
 for(unsigned i=0;i<FIFO_SNAPSHOT_COUNT;i++)if(address==0xa90000u+fifo_snapshot_offset(i)){index=i;break;}
 if(!value||index==FIFO_SNAPSHOT_COUNT)return -5;
 uint64_t par=uefi_page_translation(address&~0xfffULL);
 print("geni-preflight map address %x PAR %x\n",address,par);
 if((par&1)||(par&0x0000fffffffff000ULL)!=(address&~0xfffULL)||((par>>56)&0xf0))return -3;
 op13_trace(36,index*2);
 __asm__ volatile("dsb sy" ::: "memory");
 *value=*(volatile uint32_t *)(uintptr_t)address;
 __asm__ volatile("dsb sy" ::: "memory");
 op13_trace(36,index*2+1);
 /* Stage 48+index carries the raw value immediately over USB INFO. */
 op13_trace(48+index,*value);
 return 0;
}
static void geni_preflight_diag(void)
{
 struct sm8750_config_query_report cfg={0};
 (void)sm8750_loader_config_query(BS,1,&cfg);
 if(!cfg.derived_matched_a90000||cfg.se_wrapper_base!=0xa80000||
    cfg.se_index!=4||cfg.wrapper_common_base!=0xac0000){
  print("geni-preflight refused topology\n");return;
 }
 int rc=geni_preflight_run(BS,1,geni_read_allowlist,0,&geni_state);
 print("geni-preflight rc %x reads %d votes %x cleanup %x IF_DISABLE %x FW_REV %x\n",
  (uint64_t)(int64_t)rc,(uint64_t)geni_state.reads,(uint64_t)geni_state.votes,
  (uint64_t)(int64_t)geni_state.cleanup_result,(uint64_t)geni_state.if_disable,(uint64_t)geni_state.fw_revision);
 for(unsigned i=0;i<5;i++)print("geni-preflight clock %d id %x get-id %x initial-enabled %d enabled-query %x enable %x on %d on-query %x disable %x\n",
  (uint64_t)i,geni_state.ids[i],geni_state.id_status[i],(uint64_t)geni_state.initial_enabled[i],
  geni_state.enabled_status[i],geni_state.enable_status[i],(uint64_t)geni_state.on[i],
  geni_state.on_status[i],geni_state.disable_status[i]);
 print("fifo-snapshot complete %d stable %d local-idle %d protocol-spi %d result %x clean %d\n",
  (uint64_t)geni_state.snapshot.completed,(uint64_t)geni_state.snapshot.stable,
  (uint64_t)geni_state.snapshot.local_idle,(uint64_t)geni_state.snapshot.protocol_spi,
  (uint64_t)(int64_t)geni_state.snapshot.result,
  (uint64_t)(geni_state.votes==0&&geni_state.cleanup_result==0));
 for(unsigned i=0;i<FIFO_SNAPSHOT_COUNT;i++)print("fifo-snapshot reg %x first %x second %x\n",
  (uint64_t)(0xa90000u+fifo_snapshot_offset(i)),(uint64_t)geni_state.snapshot.first[i],
  (uint64_t)geni_state.snapshot.second[i]);
}
