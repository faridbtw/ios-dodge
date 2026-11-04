/* UEFI-only menu binding. The existing touch preparation owns all snapshots.
 * Stop/restore before any mutating host command or XNU handoff. */
#include "s3910.h"
static struct s3910 menu_touch_controller;
static unsigned menu_touch_owned, menu_touch_ready, menu_key_events;
static uint64_t menu_last_poll, menu_keys_after;
struct menu_input_key { uint16_t scan, unicode; };
struct menu_text_input {
    void *reset;
    efi_status (EFIAPI *read_key)(struct menu_text_input *, struct menu_input_key *);
    void *wait_for_key;
};
static int menu_spi_read(void *c,uint8_t *p,size_t n)
{(void)c;return op13_bitbang_transfer(&bb_state,0,p,n);}
static int menu_spi_write(void *c,const uint8_t *p,size_t n)
{(void)c;return op13_bitbang_transfer(&bb_state,p,0,n);}
static int menu_irq(void *c)
{(void)c;uint32_t v;if(!menu_touch_owned||sm8750_panel_raw_read(0,0xa2004,&v))return -1;return !(v&1);}
static void menu_sleep(void *c,unsigned ms)
{(void)c;wdt_pet();if(ms<=1000)BS->stall((uint64_t)ms*1000);}
static unsigned menu_log_drag,menu_log_last_y,menu_log_scroll_events;
static int menu_log_drag_pixels;
static void menu_touch_event(void *c,unsigned type,unsigned slot,uint32_t x,uint32_t y)
{
    (void)c;if(slot>=10||x>65535||y>65535)return;
    unsigned bit=1u<<slot;
    unsigned py=(uint64_t)y*(boot_menu.screen_height-1)/65535;
    if(type==1&&!boot_menu.contacts&&boot_menu.visible&&boot_menu.show_logs&&
       py>=op13_ui_log_top(&boot_menu)&&py<op13_ui_log_bottom(&boot_menu)&&
       op13_menu_hit(&boot_menu,x,y)==OP13_MENU_NONE){
        menu_log_drag=bit;menu_log_last_y=py;menu_log_drag_pixels=0;
    }
    op13_menu_touch(&boot_menu,type,slot,x,y);
    if(boot_menu.contacts&~menu_log_drag)menu_log_drag=0; /* cancel multi-touch */
    if(type==2&&menu_log_drag==bit&&boot_menu.show_logs){
        menu_log_drag_pixels+=(int)py-(int)menu_log_last_y;menu_log_last_y=py;
        int lines=menu_log_drag_pixels/OP13_CELL_HEIGHT;
        if(lines){
            menu_log_drag_pixels-=lines*OP13_CELL_HEIGHT;
            if(op13_console_scroll(&big_console,lines)){
                menu_log_scroll_events++;
                console_dirty_top=op13_ui_log_top(&boot_menu)+16;
                console_dirty_bottom=op13_ui_log_bottom(&boot_menu)-16;
                screen_log_flush(1);
            }
        }
    }
    if(type==3&&menu_log_drag==bit)menu_log_drag=0;
}
static int boot_menu_stop(void)
{
    menu_log_drag=0;menu_log_drag_pixels=0;
    op13_menu_disable(&boot_menu); /* release_all must never activate a button */
    boot_menu.ios_ready=0;
    boot_menu_draw();screen_log_flush(1); /* Keep uploads behind the cached UI. */
    if(!menu_touch_owned)return 0;
    menu_touch_ready=0;
    s3910_release_all(&menu_touch_controller);
    int rc=bb_cleanup_all();
    if(!rc){menu_touch_owned=0;touch_runtime_clear();}
    else identify_session.blocked=1;
    print("menu: touch released rc %x\n",(uint64_t)(int64_t)rc);
    return rc;
}
static void boot_menu_start(void)
{
    if(PLAT!=&platform_dodge||!big_console_ready||!boot_menu.visible||running_live)return;
    /* Discard any boot/reboot key. A fresh volume selection is required. */
    struct menu_text_input *in=ST->con_in;
    struct menu_input_key key;
    if(in&&in->read_key)for(unsigned i=0;i<32;i++)if(EFI_ERROR(in->read_key(in,&key)))break;
    menu_keys_after=READ_SYSREG(CNTVCT_EL0)+READ_SYSREG(CNTFRQ_EL0);
    int rc=touch_runtime_arm();
    if(!rc){
        menu_touch_owned=1;
        struct s3910_io io={0,menu_spi_read,menu_spi_write,menu_irq,menu_sleep,menu_touch_event};
        rc=s3910_probe(&menu_touch_controller,&io);
        if(!rc)rc=s3910_enable_lbp(&menu_touch_controller);
        if(!rc)menu_touch_ready=1;
        else {
            s3910_release_all(&menu_touch_controller);
            int cleanup=bb_cleanup_all();
            if(!cleanup){menu_touch_owned=0;touch_runtime_clear();}
            else identify_session.blocked=1;
        }
    }
    print("menu: Return to Android; keys %d touch-ready %d rc %x\n",
          (uint64_t)(in&&in->read_key),(uint64_t)menu_touch_ready,(uint64_t)(int64_t)rc);
    boot_menu_draw();
    screen_log_flush(1);
}
static void boot_menu_poll(void)
{
    if(!boot_menu.visible||running_live||screen_quiet)return;
    uint64_t now=READ_SYSREG(CNTVCT_EL0),freq=READ_SYSREG(CNTFRQ_EL0);
    if(now-menu_last_poll<freq/200)return;
    menu_last_poll=now;
    unsigned old_focus=boot_menu.focus,old_capture=boot_menu.captured;
    struct menu_text_input *in=ST->con_in;
    struct menu_input_key key;
    if(in&&in->read_key&&!EFI_ERROR(in->read_key(in,&key))&&now>menu_keys_after){
        if(menu_key_events++<16)print("menu: key scan %x unicode %x\n",(uint64_t)key.scan,(uint64_t)key.unicode);
        op13_menu_key(&boot_menu,key.scan,key.unicode);
    }
    if(menu_touch_ready){
        int rc=s3910_poll(&menu_touch_controller);
        if(rc){
            print("menu: touch poll stopped rc %x\n",(uint64_t)(int64_t)rc);
            int cleanup=boot_menu_stop();
            if(!cleanup){op13_menu_init(&boot_menu,SCREEN.width,SCREEN.height);boot_menu.uploading=menu_upload_active;boot_menu_draw();screen_log_flush(1);}
            return;
        }
    }
    if(old_focus!=boot_menu.focus||old_capture!=boot_menu.captured){
        boot_menu_draw();
        screen_log_flush(1);
    }
}

/* A display-only action is safe inside an active receive. Never cancel RX. */
static void boot_menu_toggle_logs(void)
{
    if(boot_menu.action!=OP13_MENU_LOGS||running_live)return;
    boot_menu.action=0;boot_menu.show_logs=!boot_menu.show_logs;
    if(menu_upload_active)menu_upload_toggles++; /* Count silently; no toggle spam. */
    boot_menu_draw();screen_log_flush(1);
}
/* Called outside USB receive/transmit waits, after the old RX was aborted. */
static void boot_menu_dispatch(void)
{
    unsigned action=boot_menu.action;
    if(!action||running_live)return;
    if(action==OP13_MENU_LOGS){boot_menu_toggle_logs();return;}
    if(menu_upload_active){boot_menu.action=0;return;}
    if(action==OP13_MENU_ANDROID){
        op13_menu_disable(&boot_menu);
        print("menu: returning to Android\n");screen_log_flush(1);
        ST->rt->reset_system(0,0,0,NULL);
        for(;;){}
    }
    if(action!=OP13_MENU_IOS||!menu_ios_prepared){boot_menu.action=0;return;}
    if(boot_menu_stop()){
        menu_ios_prepared=0;
        print("menu: cannot launch iOS; touch cleanup failed\n");
        return;
    }
    menu_ios_prepared=0;
    print("menu: Boot iOS selected; arming fresh XNU touch lease\n");
    int rc=touch_runtime_arm();
    if(rc){
        print("menu: iOS touch preparation failed rc %x\n",(uint64_t)(int64_t)rc);
        op13_menu_init(&boot_menu,SCREEN.width,SCREEN.height);
        boot_menu_draw();screen_log_flush(1);return;
    }
    last_kernel_log=NULL;last_kernel_log_len=0;
    set_result("running",0,0);
    print("== XNULOADER run (menu) ==\n");
    screen_log_flush(1);
    if(!run_kernel(1,0)){
        set_result("not-started",0,0);
        touch_runtime_after_return();
        screen_quiet=0;
        op13_menu_init(&boot_menu,SCREEN.width,SCREEN.height);
        boot_menu_draw();screen_log_flush(1);
    }
}
