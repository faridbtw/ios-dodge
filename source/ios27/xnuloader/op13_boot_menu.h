/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef OP13_BOOT_MENU_H
#define OP13_BOOT_MENU_H
#include <stdint.h>
enum { OP13_MENU_NONE, OP13_MENU_IOS, OP13_MENU_ANDROID, OP13_MENU_LOGS };
struct op13_menu {
    uint32_t left, top, width, height, screen_width, screen_height, gap, scale, logs_left, logs_top, logs_width, logs_height;
    unsigned visible, focus, contacts, captured, cancelled, action, target, ios_ready, show_logs, uploading;
};
static int op13_menu_init(struct op13_menu *m, unsigned width, unsigned height)
{
    *m = (struct op13_menu){0};
    if (width < 640 || height < 600 || width > 8192 || height > 8192) return 0;
    m->screen_width=width;m->screen_height=height;
    m->scale=width/360;if(!m->scale)m->scale=1;if(m->scale>4)m->scale=4;
    while(m->scale>1&&height<210*m->scale+64)m->scale--;
    m->left=width/12;m->width=width-2*m->left;m->height=54*m->scale;m->gap=6*m->scale;
    m->top=height-96*m->scale-2*m->height-m->gap;m->visible=1;
    m->logs_width=70*m->scale;m->logs_height=24*m->scale;
    m->logs_left=width-m->left-m->logs_width;m->logs_top=24*m->scale;
    return 1;
}
static unsigned op13_menu_hit(const struct op13_menu *m,uint32_t nx,uint32_t ny)
{
    if(!m->visible||nx>65535||ny>65535)return OP13_MENU_NONE;
    uint32_t x=(uint64_t)nx*(m->screen_width-1)/65535;
    uint32_t y=(uint64_t)ny*(m->screen_height-1)/65535;
    if(x>=m->logs_left&&x<m->logs_left+m->logs_width&&y>=m->logs_top&&y<m->logs_top+m->logs_height)return OP13_MENU_LOGS;
    if(m->uploading)return OP13_MENU_NONE;
    if(x<m->left||x>=m->left+m->width)return OP13_MENU_NONE;
    if(y>=m->top&&y<m->top+m->height)return OP13_MENU_IOS;
    if(y>=m->top+m->height+m->gap&&y<m->top+m->height+m->gap+m->height)return OP13_MENU_ANDROID;
    return OP13_MENU_NONE;
}
static void op13_menu_activate(struct op13_menu *m,unsigned target)
{
    if(m->uploading&&target!=OP13_MENU_LOGS)return;
    if(target==OP13_MENU_LOGS||target==OP13_MENU_ANDROID||(target==OP13_MENU_IOS&&m->ios_ready))m->action=target;
}
static void op13_menu_touch(struct op13_menu *m,unsigned type,unsigned slot,uint32_t x,uint32_t y)
{
    if(!m->visible||slot>=10)return;
    unsigned bit=1u<<slot;
    if(type==1){
        if(m->contacts)m->cancelled=1;
        else{m->cancelled=0;m->target=op13_menu_hit(m,x,y);m->captured=m->target?bit:0;}
        m->contacts|=bit;
    }else if(type==2&&m->captured==bit&&op13_menu_hit(m,x,y)!=m->target)m->cancelled=1;
    else if(type==3){
        if((m->contacts&bit)&&m->captured==bit&&!m->cancelled&&op13_menu_hit(m,x,y)==m->target)
            op13_menu_activate(m,m->target);
        m->contacts&=~bit;
        if(!m->contacts){m->captured=0;m->cancelled=0;m->target=0;}
    }
}
static void op13_menu_key(struct op13_menu *m,unsigned scan,unsigned unicode)
{
    if(!m->visible)return;
    if(m->uploading){
        if(scan==1||scan==2)m->focus=OP13_MENU_LOGS;
        else if(scan==0x17)m->focus=0;
        else if(m->focus==OP13_MENU_LOGS&&(unicode==13||scan==0x102))op13_menu_activate(m,OP13_MENU_LOGS);
        return;
    }
    if(scan==1)m->focus=!m->focus?OP13_MENU_IOS:(m->focus==OP13_MENU_IOS?OP13_MENU_LOGS:m->focus-1);
    else if(scan==2)m->focus=!m->focus?OP13_MENU_ANDROID:(m->focus==OP13_MENU_LOGS?OP13_MENU_IOS:m->focus+1);
    else if(scan==0x17)m->focus=0;
    else if(m->focus&&(unicode==13||scan==0x102))op13_menu_activate(m,m->focus);
}
static void op13_menu_disable(struct op13_menu *m)
{m->uploading=m->visible=m->focus=m->contacts=m->captured=m->cancelled=m->action=m->target=0;}
#endif
