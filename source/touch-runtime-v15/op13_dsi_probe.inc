#include "op13_dsi_brightness.h"
struct op13_brightness_cpu {
 void *slots[7];
 efi_status (*attrs)(struct op13_brightness_cpu *,uint64_t,uint64_t,uint64_t);
};
_Static_assert(offsetof(struct op13_brightness_cpu,attrs)==0x38,"CPU attribute ABI");
static int op13_dsi_mapped,op13_dsi_poisoned;
static uint32_t op13_dsi_read(void *c,unsigned o)
{
 uint64_t base=(uint64_t)(uintptr_t)c;
 if((base!=0x0ae94000&&base!=0x0ae96000)||(o&3)||o>=0x1000)return ~0u;
 uint32_t v=*(volatile uint32_t *)(uintptr_t)(base+o);
 __asm__ volatile("dsb sy":::"memory");return v;
}
static void op13_dsi_write(void *c,unsigned o,uint32_t v)
{
 uint64_t base=(uint64_t)(uintptr_t)c;const unsigned allowed[]={4,0x3c,0x4c,0x84,0x90,0x110,0x15c,0x17c,0x1ec};int ok=0;
 for(unsigned i=0;i<sizeof(allowed)/sizeof(*allowed);i++)if(allowed[i]==o)ok=1;
 if(base!=0x0ae94000||!ok||op13_dsi_poisoned)return;
 *(volatile uint32_t *)(uintptr_t)(base+o)=v;__asm__ volatile("dsb sy":::"memory");
}
static void op13_dsi_delay(void *c,unsigned us){(void)c;BS->stall(us);}
static int op13_dsi_map(void)
{
 if(op13_dsi_mapped)return 0;
 struct op13_brightness_cpu *cpu=0;const efi_guid guid=EFI_CPU_ARCH_GUID;
 efi_status s=BS->locate_protocol(&guid,0,(void **)&cpu);if(EFI_ERROR(s)||!cpu||!cpu->attrs)return -1;
 for(uint64_t base=0x0ae94000;base<=0x0ae94000;base+=0x2000){
  uint64_t before=uefi_page_translation(base);
  if(!(before&1)&&(before&0x000ffffffffff000ULL)!=base)return -2;
  s=cpu->attrs(cpu,base,4096,0x4001);uint64_t after=uefi_page_translation(base);
  print("op13-dsi: map base=%x status=%x PAR=%x\n",base,s,after);
  if(EFI_ERROR(s)||(after&1)||(after&0x000ffffffffff000ULL)!=base)return -3;
 }
 op13_dsi_mapped=1;return 0;
}
struct op13_display_clock {
 uint64_t revision;
 efi_status (*get_id)(struct op13_display_clock *,const char *,uint64_t *);
 efi_status (*enable)(struct op13_display_clock *,uint64_t);
 void *disable, *is_enabled;
 efi_status (*is_on)(struct op13_display_clock *,uint64_t,uint8_t *);
 void *set_frequency;
 efi_status (*get_frequency)(struct op13_display_clock *,uint64_t,uint32_t *);
};
_Static_assert(offsetof(struct op13_display_clock,enable)==0x10,"clock enable ABI");
static int op13_display_clock_query(int vote)
{
 const efi_guid guid={0x241afae6,0x885f,0x4f6c,{0xa7,0xea,0xc2,0x8e,0xab,0x79,0xc3,0xe5}};
 struct op13_display_clock *p=0;
 efi_status s=BS->locate_protocol(&guid,0,(void **)&p);
 print("op13-dsi: clock locate=%x revision=%x\n",s,p?p->revision:0);
 if(EFI_ERROR(s)||!p||p->revision!=0x1000b||!p->get_id||!p->is_on||!p->get_frequency||!p->enable)return -10;
 const char *names[]={"disp_cc_mdss_ahb_clk","disp_cc_mdss_ahb1_clk","disp_cc_mdss_non_gdsc_ahb_clk","gcc_disp_ahb_clk","disp_cc_mdss_byte0_clk","disp_cc_mdss_esc0_clk"};
 for(unsigned i=0;i<sizeof(names)/sizeof(*names);i++){
  uint64_t id=0;uint32_t hz=0;uint8_t on=0;
  efi_status a=p->get_id(p,names[i],&id),b=~0ULL,c=~0ULL;
  if(!EFI_ERROR(a)){b=p->get_frequency(p,id,&hz);c=p->is_on(p,id,&on);}
  print("op13-dsi: clock %s lookup=%x id=%x rate-status=%x hz=%d on-status=%x on=%d\n",names[i],a,id,b,(uint64_t)hz,c,(uint64_t)on);
  /* Explicit diagnostic argument votes only the configuration AHB clock.
   * Never change frequency, panel power, pixel, byte, or escape clocks. */
  if(vote&&i==0){
   if(EFI_ERROR(a)||EFI_ERROR(c))return -11;
   efi_status en=p->enable(p,id);BS->stall(10);on=0;c=p->is_on(p,id,&on);
   print("op13-dsi: config-AHB vote=%x on-status=%x on=%d\n",en,c,(uint64_t)on);
   if(EFI_ERROR(en)||EFI_ERROR(c)||!on)return -12;
  }
 }
 return 0;
}
static void op13_dsi_probe(const char *arg)
{
 if(op13_dsi_poisoned){fb_reply("FAIL","Previous DSI failure requires cold reset");return;}
 efi_status status=BS->set_watchdog_timer(15,0x10000,0,0);if(EFI_ERROR(status)){fb_reply("FAIL","Watchdog unavailable");return;}
 while(*arg==' ')arg++;
 int vote=str_eq(arg,"ahb");
 int rc=op13_display_clock_query(vote);
 if(vote)arg="";
 if(!rc)rc=op13_dsi_map();
 if(!rc)for(unsigned ctrl=0;ctrl<1;ctrl++){
  void *ctx=(void *)(uintptr_t)(0x0ae94000+ctrl*0x2000);
  print("op13-dsi: controller=%d version=%x CTRL=%x STATUS=%x DMA=%x TRIG=%x INT=%x TPG=%x\n",(uint64_t)ctrl,
   (uint64_t)op13_dsi_read(ctx,0),(uint64_t)op13_dsi_read(ctx,4),(uint64_t)op13_dsi_read(ctx,8),
   (uint64_t)op13_dsi_read(ctx,0x3c),(uint64_t)op13_dsi_read(ctx,0x84),(uint64_t)op13_dsi_read(ctx,0x110),(uint64_t)op13_dsi_read(ctx,0x15c));
 }
 while(*arg==' ')arg++;
 if(!rc&&*arg){
  unsigned pct=0,count=0;while(arg[count]>='0'&&arg[count]<='9'&&count<3){pct=pct*10+arg[count]-'0';count++;}
  if(!count||arg[count]||pct<10||pct>90)rc=-2;
  else{
   struct op13_dsi_io io={(void *)(uintptr_t)0x0ae94000,op13_dsi_read,op13_dsi_write,op13_dsi_delay};
   unsigned dbv=(3515*pct+50)/100;rc=op13_dsi_set(&io,dbv);
   print("op13-dsi: DCS51 percent=%d DBV=%d result=%d; controller state restored on completion\n",(uint64_t)pct,(uint64_t)dbv,(uint64_t)(int64_t)rc);
   if(rc<=-4)op13_dsi_poisoned=1;
  }
 }
 BS->set_watchdog_timer(0,0,0,0);fb_reply(rc?"FAIL":"OKAY",rc?"Inspect DSI diagnostic log":"");
}
