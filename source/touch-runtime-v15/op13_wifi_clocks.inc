/* Dodge PCIe0 preflight. Getter-only: no clock, power, reset or MMIO writes.
 * Prefix ABI matches the existing verified SM8750 ClockDxe v0x1000b adapter.
 * The power-domain getter is the public EFIClock.h slot at offset 0x50.
 */
struct op13_wifi_clock_protocol {
 uint64_t revision;
 efi_status (*get_id)(struct op13_wifi_clock_protocol *,const char *,uint64_t *);
 void *enable, *disable, *is_enabled;
 efi_status (*is_on)(struct op13_wifi_clock_protocol *,uint64_t,uint8_t *);
 void *set_frequency;
 efi_status (*get_frequency)(struct op13_wifi_clock_protocol *,uint64_t,uint32_t *);
 void *calc_frequency, *select_external;
 efi_status (*get_power_id)(struct op13_wifi_clock_protocol *,const char *,uint64_t *);
};
_Static_assert(offsetof(struct op13_wifi_clock_protocol,is_on)==0x28,"clock state ABI");
_Static_assert(offsetof(struct op13_wifi_clock_protocol,get_frequency)==0x38,"clock rate ABI");
_Static_assert(offsetof(struct op13_wifi_clock_protocol,get_power_id)==0x50,"power ID ABI");
static void
op13_wifi_clocks(void)
{
 static const efi_guid guid={0x241afae6,0x885f,0x4f6c,{0xa7,0xea,0xc2,0x8e,0xab,0x79,0xc3,0xe5}};
 static const char *const names[]={
  "gcc_pcie_0_aux_clk", "gcc_pcie_0_cfg_ahb_clk", "gcc_pcie_0_mstr_axi_clk",
  "gcc_pcie_0_slv_axi_clk", "gcc_pcie_0_slv_q2a_axi_clk", "gcc_pcie_0_pipe_clk",
  "gcc_pcie_0_phy_rchng_clk", "gcc_pcie_rscc_ahb_clk", "gcc_pcie_rscc_xo_clk",
  "pcie_ahb_clk", "pcie_anoc_ahb_clk", "pcie_axi_clk", "pcie_qtb_clk",
  "pcie_sf_axi_clk", "pcie_sf_qtb_clk", "pcie_sf_tbu_clk", "pcie_0_clkref_en"
 };
 struct op13_wifi_clock_protocol *p=0;
 efi_status status=BS->locate_protocol(&guid,0,(void **)&p);
 print("op13-wifi-clocks: locate=%x revision=%x\n",status,p?p->revision:0);
 if(EFI_ERROR(status)||!p||p->revision!=0x1000b||!p->get_id||!p->is_on||!p->get_frequency) {
  print("op13-wifi-clocks: unsupported getter ABI; no calls\n"); return;
 }
 for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++) {
  uint64_t id=0;uint32_t rate=0;uint8_t on=0;
  efi_status a=p->get_id(p,names[i],&id),b=UINT64_MAX,c=UINT64_MAX;
  if(!EFI_ERROR(a)){b=p->get_frequency(p,id,&rate);c=p->is_on(p,id,&on);}
  print("op13-wifi-clocks: %s id-status=%x id=%x rate-status=%x hz=%d on-status=%x on=%d\n",
    names[i],a,id,b,(uint64_t)rate,c,(uint64_t)on);
 }
 /* Defer the extended power-domain slot until its exact binary ABI is checked. */
 print("op13-wifi-clocks: complete; no resource votes or hardware writes\n");
}
