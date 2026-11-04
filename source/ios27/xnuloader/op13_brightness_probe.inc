/* Native DisplayPwrCtrl ABI, revision10000. Standalone RAM diagnostic only.
 * Never changes panel power, mode, clocks, refresh rate, or DSI registers.
 */
struct op13_display_power {
 uint64_t revision;
 void *power_control,*power_status;
 efi_status (*brightness_set)(uint32_t,uint32_t);
 efi_status (*brightness_get)(uint32_t,uint32_t *);
};
static void op13_brightness_probe(const char *arg)
{
 const efi_guid guid={0x7bfa4293,0x7aa4,0x4375,{0xb6,0x3c,0xb6,0xaa,0xb7,0x86,0xc4,0x3c}};
 struct op13_display_power *p=0;uint32_t initial=~0u;
 efi_status rc=BS->locate_protocol(&guid,0,(void **)&p);
 print("op13-brightness: locate=%x revision=%x\n",rc,p?p->revision:0);
 if(EFI_ERROR(rc)||!p||p->revision!=0x10000||!p->brightness_get||!p->brightness_set){fb_reply("FAIL","Native brightness protocol unavailable or unsupported ABI");return;}
 rc=p->brightness_get(0,&initial);
 print("op13-brightness: primary getter=%x percent=%d\n",rc,(uint64_t)initial);
 if(EFI_ERROR(rc)||initial>100){fb_reply("FAIL","Native brightness query failed; nothing changed");return;}
 while(*arg==' ')arg++;
 if(!*arg){fb_reply("OKAY","");return;}
 if(!starts_with(arg,"test")||arg[4]){fb_reply("FAIL","Only query or bounded test supported");return;}
 rc=BS->set_watchdog_timer(15,0x10000,0,0);
 if(EFI_ERROR(rc)){fb_reply("FAIL","Watchdog unavailable; nothing changed");return;}
 const uint32_t levels[]={20,70};int failed=0;
 for(unsigned i=0;i<2;i++){
  uint32_t observed=~0u;rc=p->brightness_set(0,levels[i]);
  efi_status check=p->brightness_get(0,&observed);
  print("op13-brightness: test requested=%d setter=%x getter=%x observed=%d\n",(uint64_t)levels[i],rc,check,(uint64_t)observed);
  if(EFI_ERROR(rc)||EFI_ERROR(check)||observed!=levels[i]){failed=1;break;}
  BS->stall(2000000);
 }
 /* Restore the exact original level even after a failed setter/readback. */
 rc=p->brightness_set(0,initial);
 print("op13-brightness: restore=%d status=%x\n",(uint64_t)initial,rc);
 BS->set_watchdog_timer(0,0,0,0);
 fb_reply(failed||EFI_ERROR(rc)?"FAIL":"OKAY",failed?"Inspect native brightness log":"");
}
