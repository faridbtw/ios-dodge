/* Dodge PCIe0 queries plus an explicit, reversible power/clock preflight.
 * Prefix ABI matches the existing verified SM8750 ClockDxe v0x1000b adapter.
 * The power-domain getter is the public EFIClock.h slot at offset 0x50.
 */
struct op13_wifi_clock_protocol {
 uint64_t revision;
 efi_status (*get_id)(struct op13_wifi_clock_protocol *,const char *,uint64_t *);
 efi_status (*enable)(struct op13_wifi_clock_protocol *,uint64_t);
 efi_status (*disable)(struct op13_wifi_clock_protocol *,uint64_t);
 void *is_enabled;
 efi_status (*is_on)(struct op13_wifi_clock_protocol *,uint64_t,uint8_t *);
 void *set_frequency;
 efi_status (*get_frequency)(struct op13_wifi_clock_protocol *,uint64_t,uint32_t *);
 void *calc_frequency, *select_external;
 efi_status (*get_power_id)(struct op13_wifi_clock_protocol *,const char *,uint64_t *);
 efi_status (*enable_power)(struct op13_wifi_clock_protocol *,uint64_t);
 efi_status (*disable_power)(struct op13_wifi_clock_protocol *,uint64_t);
};
_Static_assert(offsetof(struct op13_wifi_clock_protocol,is_on)==0x28,"clock state ABI");
_Static_assert(offsetof(struct op13_wifi_clock_protocol,get_frequency)==0x38,"clock rate ABI");
_Static_assert(offsetof(struct op13_wifi_clock_protocol,get_power_id)==0x50,"power ID ABI");
static const efi_guid op13_wifi_clock_guid={0x241afae6,0x885f,0x4f6c,{0xa7,0xea,0xc2,0x8e,0xab,0x79,0xc3,0xe5}};
static int op13_wifi_votes_stuck;
static void (*op13_wifi_survey_callback)(void);
static void
op13_wifi_clocks(void)
{
 static const char *const names[]={
  "gcc_pcie_0_aux_clk", "gcc_pcie_0_cfg_ahb_clk", "gcc_pcie_0_mstr_axi_clk",
  "gcc_pcie_0_slv_axi_clk", "gcc_pcie_0_slv_q2a_axi_clk", "gcc_pcie_0_pipe_clk",
  "gcc_pcie_0_phy_rchng_clk", "gcc_pcie_rscc_ahb_clk", "gcc_pcie_rscc_xo_clk",
  "pcie_ahb_clk", "pcie_anoc_ahb_clk", "pcie_axi_clk", "pcie_qtb_clk",
  "pcie_sf_axi_clk", "pcie_sf_qtb_clk", "pcie_sf_tbu_clk", "pcie_0_clkref_en"
 };
 struct op13_wifi_clock_protocol *p=0;
 efi_status status=BS->locate_protocol(&op13_wifi_clock_guid,0,(void **)&p);
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
 /* Exact ClockDxe table at file0x28148: power ID and ordinary ID share RVA156c. */
 if(p->get_power_id && p->get_power_id==p->get_id) {
  const char *domains[]={"gcc_pcie_0_gdsc","gcc_pcie_0_phy_gdsc"};
  for(unsigned i=0;i<2;i++) {
   uint64_t id=0;efi_status s=p->get_power_id(p,domains[i],&id);
   print("op13-wifi-clocks: power-domain %s status=%x id=%x\n",domains[i],s,id);
  }
 }
 print("op13-wifi-clocks: complete; no resource votes or hardware writes\n");
}

/* Explicit, bounded voting test. No PCIe, PHY, radio or DMA MMIO is accessed.
 * Do not enable PIPE before the controller's PHY has been initialized.
 * An ambiguous enable/disable failure requires cold reset, never kernel launch.
 */
static void
op13_wifi_power_preflight(void)
{
 static const char *const names[]={"gcc_pcie_0_gdsc","gcc_pcie_0_phy_gdsc",
  "gcc_pcie_0_cfg_ahb_clk","gcc_pcie_0_aux_clk","gcc_pcie_0_phy_rchng_clk",
  "gcc_pcie_0_mstr_axi_clk","gcc_pcie_0_slv_axi_clk","gcc_pcie_0_slv_q2a_axi_clk"};
 struct op13_wifi_clock_protocol *p=0;uint64_t ids[8]={0};unsigned acquired=0;
 efi_status s=BS->locate_protocol(&op13_wifi_clock_guid,0,(void **)&p);
 if(op13_wifi_votes_stuck||EFI_ERROR(s)||!p||p->revision!=0x1000b||!p->get_id||!p->enable||!p->disable||!p->is_on||
    p->get_power_id!=p->get_id||p->enable_power!=p->enable||p->disable_power!=p->disable) {
  print("op13-wifi-power: refused unsupported ABI or retained votes\n");return;
 }
 for(unsigned i=0;i<8;i++) {
  s=p->get_id(p,names[i],&ids[i]);
  print("op13-wifi-power: resolve %s status=%x id=%x\n",names[i],s,ids[i]);
  if(EFI_ERROR(s)) return;
  for(unsigned j=0;j<i;j++) if(ids[i]==ids[j]) {
   print("op13-wifi-power: duplicate resource ID; no votes\n");return;
  }
 }
 for(unsigned i=0;i<8;i++) {
  s=p->enable(p,ids[i]);
  print("op13-wifi-power: enable %s status=%x\n",names[i],s);
  if(EFI_ERROR(s)) {op13_wifi_votes_stuck=1;print("op13-wifi-power: ambiguous enable; cold reset required\n");return;}
  acquired++;
  if(i>=2) {
   uint8_t on=0;efi_status q=p->is_on(p,ids[i],&on);
   print("op13-wifi-power: state %s status=%x on=%d\n",names[i],q,(uint64_t)on);
   if(EFI_ERROR(q)||!on) break;
  }
  if(i==4 && op13_wifi_survey_callback) {
   op13_wifi_survey_callback();
   if(op13_wifi_votes_stuck) return;
  }
 }
 unsigned checked=acquired;
 while(acquired) {
  unsigned i=acquired-1;s=p->disable(p,ids[i]);
  print("op13-wifi-power: release %s status=%x\n",names[i],s);
  if(EFI_ERROR(s)){op13_wifi_votes_stuck=1;print("op13-wifi-power: release failed; cold reset required\n");return;}
  acquired--;
 }
 print("op13-wifi-power: preflight ended resources=%d; all acquired votes released\n",(uint64_t)checked);
}
