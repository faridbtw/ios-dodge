/* SPDX-License-Identifier: BSD-2-Clause
 * Dodge PCIe0 root/link/config probe. No firmware download, DMA or MSI setup.
 * Register definitions/order: pinned vendor-pcie/pci-msm.c; rails/pins: phone DT.
 * Retain all client votes and controller state until a cold reset.
 */
static int op13_wifi_link_ready,op13_wifi_link_fault;
static int op13_wifi_link_address(uint32_t a,int write)
{
 static const uint32_t parf[]={0x1000,0x350,0x354,0x358,0x35c,0x634,0x638,0x1a8,0x22c,0x2c4,0x1b0,0x2c48};
 static const uint32_t dbi[]={0,4,8,0xc,0x18,0x7c,0x80,0xa0,0x70c,0x714,0x80c,0x8bc,0xb40};
 if(a&3)return 0;
 if(!write&&(a==0x1c00000||a==0x1c00040||a==0x1c00174||a==0x40000710||a==0x40000890||a==0x400008a8||a==0x400008ac||a==0x16b000||a==0x16c01c||a==0x16b054||a==0x16b080))return 1;

 if(a==0x0f204008)return 1; /* DT sun-tcsrcc base, PCIe0 clkref only. */
 for(unsigned i=0;i<sizeof(parf)/sizeof(*parf);i++)if(a==0x1c00000+parf[i])return 1;
 for(unsigned i=0;i<sizeof(dbi)/sizeof(*dbi);i++)if(a==0x40000000+dbi[i])return 1;
 for(unsigned i=0;i<7;i++)if(a==0x40001000+4*i)return 1;
 const unsigned pins[]={16,18,19,102,103,104,202,203};
 for(unsigned i=0;i<8;i++)if(a==0xf100000+0x1000*pins[i]||a==0xf100004+0x1000*pins[i])return 1;
 /* Endpoint configuration and ELBI status are read-only in this probe. */
 if(!write&&(a==0x40100000||a==0x40100004||a==0x40100008||a==0x40000f28))return 1;
 return 0;
}
static uint32_t op13_wifi_link_read(uint32_t a)
{
 uint64_t par=uefi_page_translation(a);
 if(!op13_wifi_link_address(a,0)||(par&1)||((par&0x000ffffffffff000ULL)!=(a&~0xfffULL))){
  op13_wifi_link_fault=1;print("op13-wifi-link: refused read=%x PAR=%x\n",(uint64_t)a,par);return ~0u;
 }
 return *(volatile uint32_t *)(uintptr_t)a;
}
static void op13_wifi_link_write(uint32_t a,uint32_t v)
{
 uint64_t par=uefi_page_translation(a);
 if(op13_wifi_link_fault||!op13_wifi_link_address(a,1)||(par&1)||((par&0x000ffffffffff000ULL)!=(a&~0xfffULL))){
  op13_wifi_link_fault=1;print("op13-wifi-link: refused write=%x PAR=%x\n",(uint64_t)a,par);return;
 }
 *(volatile uint32_t *)(uintptr_t)a=v;__asm__ volatile("dsb sy":::"memory");
}
static void op13_wifi_link_update(uint32_t a,uint32_t clear,uint32_t set)
{uint32_t v=op13_wifi_link_read(a);if(!op13_wifi_link_fault)op13_wifi_link_write(a,(v&~clear)|set);}

static int op13_wifi_link_power(void)
{
 const efi_guid cg={0x1c34f691,0xd33d,0x4e14,{0xb8,0xf8,0x32,0xc6,0xc0,0x29,0xe9,0x5b}};
 const efi_guid rg={0xe1d0b573,0xf29f,0x495d,{0x93,0x52,0xca,0x0d,0xd4,0x2a,0x0a,0x74}};
 static const struct {const char *name;uint32_t mv;} rails[]={
  {"smpd4",876},{"smpf5",876},{"smpg3",1860},{"smpi7",1312},{"ldof2",1200},{"ldof3",1800}
 };
 struct op13_slider_cmd_db *db=0;struct op13_slider_rpmh *rpm=0;
 efi_status s=BS->locate_protocol(&cg,0,(void **)&db),t=BS->locate_protocol(&rg,0,(void **)&rpm);
 if(EFI_ERROR(s)||EFI_ERROR(t)||!db||!rpm||db->revision!=0x10000||rpm->revision!=0x10001||
  !db->address||!rpm->valid||!rpm->count||!rpm->create||!rpm->issue)return -1;
 uint32_t addresses[6]={0},count=0;uint8_t valid=0;
 s=rpm->valid(2,&valid);t=rpm->count(2,&count);
 if(EFI_ERROR(s)||EFI_ERROR(t)||!valid||count<3)return -2;
 for(unsigned i=0;i<6;i++){
  s=db->address(db,rails[i].name,&addresses[i]);
  print("op13-wifi-link: rail %s address=%x status=%x target-mV=%d\n",rails[i].name,(uint64_t)addresses[i],s,(uint64_t)rails[i].mv);
  if(EFI_ERROR(s)||!addresses[i]||(addresses[i]&0xf)||((addresses[i]>>16)&7)!=4)return -3;
  for(unsigned j=0;j<i;j++)if(addresses[i]==addresses[j])return -4;
 }
 void *client=0;s=rpm->create(2,"ios-dodge-wifi",&client);
 if(EFI_ERROR(s)||!client)return -5;
 for(unsigned i=0;i<6;i++){
  const uint32_t offsets[]={0,8,4},values[]={rails[i].mv,7,1};
  for(unsigned j=0;j<3;j++){
   uint32_t request=0;s=rpm->issue(client,0,0,addresses[i]+offsets[j],values[j],&request);
   print("op13-wifi-link: vote %s offset=%d value=%d status=%x request=%d\n",rails[i].name,(uint64_t)offsets[j],(uint64_t)values[j],s,(uint64_t)request);
   if(EFI_ERROR(s))return -6;
  }
 }
 BS->stall(10000);return 0;
}
static void op13_wifi_link_probe(void)
{
 op13_wifi_link_ready=op13_wifi_link_fault=0;
 /* Match cnss_power_on_device() before PCIe host initialization. */
 if(op13_wifi_map_registers())return;
 op13_wifi_votes_stuck=1;
 if(op13_wifi_aop_pdc())return;
 /* Vendor cnss_get_pinctrl(): WCN power-feedback muxes and SOL signals. */
 print("op13-wifi-link: power feedback original GPIO18=%x GPIO19=%x\n",(uint64_t)op13_wifi_link_read(0xf112000),(uint64_t)op13_wifi_link_read(0xf113000));
 op13_wifi_link_update(0xf112000,0x3c,4); /* GPIO18 wcn_sw_ctrl function1. */
 op13_wifi_link_update(0xf113000,0x3c,4); /* GPIO19 wcn_sw function1. */
 op13_wifi_link_update(0xf1ca004,2,0);op13_wifi_link_update(0xf1ca000,0x3ff,0x241);
 op13_wifi_link_update(0xf1cb000,0x23c,0); /* DEV_SOL input; preserve pull/drive. */
 op13_wifi_link_update(0xf166004,2,0);op13_wifi_link_update(0xf166000,0x3ff,0x201);
 op13_wifi_link_update(0xf167000,0x3ff,7);op13_wifi_link_update(0xf168000,0x3ff,3);
 op13_wifi_link_update(0x0f204008,0,1);
 op13_wifi_link_update(0xf110004,2,0);op13_wifi_link_update(0xf110000,0x3ff,0x3c3);
 BS->stall(10000);
 if(op13_wifi_link_fault)return;
 int early_power=op13_wifi_link_power();
 print("op13-wifi-link: pre-host radio power=%d\n",(uint64_t)(int64_t)early_power);
 if(early_power)return;
 op13_wifi_link_update(0xf110004,0,2);BS->stall(100000);
 op13_wifi_phy_probe();
 if(!op13_wifi_phy_ready||op13_wifi_phy_fault)return;
 /* PHY probe left PERST asserted; never inspect endpoint config yet. */
 uint32_t perst=op13_wifi_link_read(0xf166004);
 if(op13_wifi_link_fault||(perst&2)){print("op13-wifi-link: endpoint reset not asserted; refusing\n");return;}
 /* Vendor disable_dbi_mirroring(): fix DBI and ATU windows before DBI access. */
 op13_wifi_link_write(0x1c00350,0x40000000);op13_wifi_link_write(0x1c00354,0);
 op13_wifi_link_write(0x1c00634,0x40001000);op13_wifi_link_write(0x1c00638,0);
 op13_wifi_link_write(0x1c00358,0);op13_wifi_link_write(0x1c0035c,0x80000000);
 op13_wifi_link_update(0x1c001a8,0,1u<<31);
 if(op13_wifi_link_fault)return;
 uint32_t root=op13_wifi_link_read(0x40000000),header=op13_wifi_link_read(0x4000000c);
 print("op13-wifi-link: root identity=%x header=%x\n",(uint64_t)root,(uint64_t)header);
 if(op13_wifi_link_fault||(root&0xffff)!=0x17cb||((header>>16)&0x7f)!=1)return;
 /* Native Gen1 configuration. Root bus-master enable remains clear. */
 op13_wifi_link_write(0x40000004,2);op13_wifi_link_write(0x40000018,0x00010100);
 op13_wifi_link_update(0x400008bc,0,1);
 op13_wifi_link_update(0x40000008,0xffffff00,0x06040000);
 op13_wifi_link_update(0x400008bc,1,0);
 op13_wifi_link_write(0x40000b40,20);op13_wifi_link_update(0x400000a0,0xf,1);
 op13_wifi_link_update(0x4000080c,0x1f00,0x100);
 op13_wifi_link_update(0x40000714,0,1u<<5);
 op13_wifi_link_update(0x1c002c4,1,1u<<8);
 op13_wifi_link_write(0x1c0022c,0); /* No firmware IRQ/MSI handlers for this controller. */
 print("op13-wifi-link: root configured; PCI bus mastering and interrupts disabled\n");
 /* OnePlus tcsrcc-sun.c @6028f47, DT clock-controller@f204008:
  * TCSR_PCIE_0_CLKREF_EN is offset0 BIT0, separate from GCC PIPE clocks. */
 print("op13-wifi-link: TCSR PCIe clkref before=%x\n",(uint64_t)op13_wifi_link_read(0x0f204008));
 op13_wifi_link_update(0x0f204008,0,1);BS->stall(1000);
 uint32_t ref=op13_wifi_link_read(0x0f204008);
 print("op13-wifi-link: TCSR PCIe clkref after=%x\n",(uint64_t)ref);
 if(op13_wifi_link_fault||!(ref&1))return;
 /* Radio is already powered. Keep it high across host setup. */
 op13_wifi_link_update(0x1c02c48,0,9);
 op13_wifi_link_update(0xf166004,0,2);BS->stall(10000);
 if(op13_wifi_link_fault)return;
 op13_wifi_link_update(0x1c001b0,0,1u<<8);
 print("op13-wifi-link: LTSSM control=%x type=%x CESTA-route=%x\n",
  (uint64_t)op13_wifi_link_read(0x1c001b0),(uint64_t)op13_wifi_link_read(0x1c01000),(uint64_t)op13_wifi_link_read(0x1c02c48));
 const unsigned pins[]={16,18,19,102,103,104,202,203};
 for(unsigned i=0;i<8;i++)print("op13-wifi-link: GPIO%d cfg=%x data=%x\n",(uint64_t)pins[i],
  (uint64_t)op13_wifi_link_read(0xf100000+pins[i]*0x1000),(uint64_t)op13_wifi_link_read(0xf100004+pins[i]*0x1000));

 const uint32_t diagnostic[]={0x1c00000,0x1c00040,0x1c00174,0x1c001a8,0x1c002c4,0x4000007c,0x40000080,0x400000a0,0x40000710,0x4000080c,0x40000890,0x400008a8,0x400008ac,0x16b000,0x16c01c,0x16b054,0x16b080};
 for(unsigned i=0;i<sizeof(diagnostic)/sizeof(*diagnostic);i++)print("op13-wifi-link: diagnostic address=%x value=%x\n",(uint64_t)diagnostic[i],(uint64_t)op13_wifi_link_read(diagnostic[i]));
 uint32_t elbi=0,status=0;
 for(unsigned i=0;i<1000;i++){
  elbi=op13_wifi_link_read(0x40000f28);status=op13_wifi_link_read(0x40000080);
  if(op13_wifi_link_fault)return;
  if((elbi&0x400)&&(status&(1u<<29)))break;
  BS->stall(1000);
 }
 print("op13-wifi-link: ELBI=%x LTSSM=%d link-status=%x\n",(uint64_t)elbi,(uint64_t)((elbi>>12)&0x3f),(uint64_t)status);
 if(!(elbi&0x400)||!(status&(1u<<29))){op13_wifi_link_update(0xf166004,2,0);return;}
 /* Outbound config region0 -> bus1/dev0/fn0; no inbound/DMA window. */
 op13_wifi_link_write(0x40001004,0);op13_wifi_link_write(0x40001000,4);
 op13_wifi_link_write(0x40001008,0x40100000);op13_wifi_link_write(0x4000100c,0);
 op13_wifi_link_write(0x40001010,0x40100fff);op13_wifi_link_write(0x40001014,0x01000000);
 op13_wifi_link_write(0x40001018,0);op13_wifi_link_write(0x40001004,0x80000000);
 if(op13_wifi_link_fault)return;
 uint32_t identity=op13_wifi_link_read(0x40100000),command=op13_wifi_link_read(0x40100004);
 print("op13-wifi-link: endpoint identity=%x command=%x; expected 110e17cb\n",(uint64_t)identity,(uint64_t)command);
 if(!op13_wifi_link_fault&&identity==0x110e17cb&&!(command&4))op13_wifi_link_ready=1;
 print("op13-wifi-link: endpoint-ready=%d; no firmware download, radio traffic or DMA performed\n",(uint64_t)op13_wifi_link_ready);
}
