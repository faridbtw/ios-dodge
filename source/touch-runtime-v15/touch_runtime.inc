/* SPDX-License-Identifier: BSD-2-Clause
 * UEFI owns supplies and the original GPIO snapshots for the whole XNU run.
 * XNU only drives the four already leased GPIOs. No EFI calls from XNU.
 * The immutable lease occupies its own 16K region, outside XNU's RAM window.
 */
/* PE/COFF clang permits at most8K static alignment. Reserve32K and choose
 * an interior16K page so XNU maps no unrelated loader data. */
static uint32_t touch_runtime_lease_storage[8192] __attribute__((aligned(0x2000)));
#define touch_runtime_lease ((uint32_t *)(((uintptr_t)touch_runtime_lease_storage+0x3fffULL)&~0x3fffULL))
static uint64_t touch_runtime_lease_address(void)
{return touch_runtime_pending?(uint64_t)(uintptr_t)touch_runtime_lease:0;}
static int touch_runtime_token(const char *s,const char *token)
{
 for(const char *c=s;*c;c++)if((c==s||c[-1]==' ')&&starts_with(c,token)){
  const char *q=c,*t=token;while(*t){q++;t++;}
  if(!*q||*q==' ')return 1;
 }
 return 0;
}
static int touch_runtime_accept(const char *cmdline)
{
 unsigned n=0;
 for(const char *c=cmdline;*c;c++){
  if(++n>BOOT_LINE_LENGTH-192||starts_with(c,"touch_lease_pa="))return 0;
 }
 return !touch_runtime_outstanding&&touch_runtime_token(cmdline,"touchscreen=2")&&
  touch_runtime_token(cmdline,"sm8750_touch_hw=1")&&
  touch_runtime_token(cmdline,"touch_steps=4");
}
static int touch_runtime_arm(void)
{
 if(touch_runtime_pending||touch_runtime_outstanding||running_live||
    identify_session.blocked||identify_session.panel_owned||bb_state.owned||
    identify_session.handle)return -6;
 if(bb_mapping_check())return -3;
 geni_preflight_diag();
 if(geni_state.result||geni_state.cleanup_result||geni_state.votes||
    !geni_state.snapshot.stable||!geni_state.snapshot.local_idle||
    !geni_state.snapshot.protocol_spi)return -3;
 identify_session.panel_owned=1;
 int rc=identify_prepare(&identify_panel);
 if(!rc){
  const struct op13_bitbang_io io={0,bb_mmio_read,bb_mmio_write,bb_delay};
  rc=op13_bitbang_open(&bb_state,&io,1);
 }
 if(rc){
  int cleanup=bb_cleanup_all();
  print("touch-runtime arm failed %x cleanup %x\n",(uint64_t)(int64_t)rc,(uint64_t)(int64_t)cleanup);
  if(cleanup)identify_session.blocked=1;
  return rc;
 }
 static const uint32_t lease[12]={0x4f503133,1,48,0x0f100000,0x100000,48,49,50,51,161,162,5};
 memcpy(touch_runtime_lease,lease,sizeof(lease));
 clean_dcache((uint64_t)(uintptr_t)touch_runtime_lease,
  (uint64_t)(uintptr_t)touch_runtime_lease+0x4000);
 touch_runtime_pending=1;
 print("touch-runtime armed lease %x SPI GPIO48/49/50/51 reset161 IRQ162; no SPI traffic yet\n",
  touch_runtime_lease_address());
 return 0;
}
static void touch_runtime_clear(void)
{
 touch_runtime_pending=0;
 memset(touch_runtime_lease,0,0x4000);
 clean_dcache((uint64_t)(uintptr_t)touch_runtime_lease,
  (uint64_t)(uintptr_t)touch_runtime_lease+0x4000);
}
static void touch_runtime_after_return(void)
{
 if(!touch_runtime_pending||touch_runtime_outstanding)return;
 int rc=bb_cleanup_all();
 print("touch-runtime kernel returned cleanup %x SPI-owned %d panel-owned %d\n",
  (uint64_t)(int64_t)rc,(uint64_t)bb_state.owned,(uint64_t)identify_session.panel_owned);
 if(!rc)touch_runtime_clear();else identify_session.blocked=1;
}
