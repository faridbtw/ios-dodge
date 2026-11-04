/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Project Boot portrait renderer, styled after UniXNU's numbered-card menu.
 * Font derived from UniXNU/EDK II; see op13_ui_font.h and bundled license.
 */
#ifndef OP13_MENU_THEME_H
#define OP13_MENU_THEME_H
#include <stddef.h>
#include "op13_ui_font.h"
struct op13_canvas {uint32_t *pixels;unsigned width,height,stride,swap;};
static uint32_t op13_ui_color(const struct op13_canvas *c,uint32_t rgb)
{return c->swap?((rgb&255)<<16)|(rgb&0xff00)|((rgb>>16)&255):rgb;}
static void op13_ui_rect(struct op13_canvas *c,unsigned x,unsigned y,unsigned w,unsigned h,uint32_t rgb)
{
 if(x>=c->width||y>=c->height)return;
 if(w>c->width-x)w=c->width-x;if(h>c->height-y)h=c->height-y;
 uint32_t color=op13_ui_color(c,rgb);
 for(unsigned j=0;j<h;j++)for(unsigned i=0;i<w;i++)c->pixels[(size_t)(y+j)*c->stride+x+i]=color;
}
static void op13_ui_text(struct op13_canvas *c,unsigned x,unsigned y,const char *s,unsigned scale,uint32_t rgb)
{
 if(!scale||scale>8||y>=c->height||19*scale>c->height-y)return;
 uint32_t color=op13_ui_color(c,rgb);
 for(unsigned i=0;s[i]&&x<c->width;i++,x+=8*scale){
  if(8*scale>c->width-x)break;
  unsigned ch=(unsigned char)s[i];if(ch<32||ch>126)ch='?';
  for(unsigned row=0;row<19*scale;row++){
   uint8_t bits=op13_ui_font[ch-32][row/scale];
   for(unsigned col=0;col<8*scale;col++)if(bits&(0x80u>>(col/scale)))
    c->pixels[(size_t)(y+row)*c->stride+x+col]=color;
  }
 }
}
static void op13_ui_center(struct op13_canvas *c,unsigned y,const char *s,unsigned scale,uint32_t rgb)
{
 unsigned n=0;while(s[n])n++;unsigned w=n*8*scale;
 if(w<=c->width)op13_ui_text(c,(c->width-w)/2,y,s,scale,rgb);
}
static unsigned op13_ui_brand_top(const struct op13_menu *m)
{return (m->screen_height-300)/2+316;}
static unsigned op13_ui_brand_bottom(const struct op13_menu *m)
{return op13_ui_brand_top(m)+19*(m->scale+1)+24+m->scale+32+19*(m->scale>1?m->scale-1:1);}
static unsigned op13_ui_logo_fits(const struct op13_menu *m)
{return m->screen_height>=1800&&op13_ui_brand_bottom(m)+32<m->top;}
static unsigned op13_ui_log_bottom(const struct op13_menu *m)
{return op13_ui_logo_fits(m)?(m->screen_height-300)/2-24:m->top-24;}
static unsigned op13_ui_log_top(const struct op13_menu *m)
{
 unsigned fs=m->scale>1?m->scale-1:1;
 unsigned end=op13_ui_logo_fits(m)?op13_ui_brand_bottom(m):128+19*(m->scale+1)+24+m->scale+32+19*fs;
 unsigned top=op13_ui_logo_fits(m)?64*m->scale+24:(end+24<m->top?end+24:64*m->scale);
 unsigned bottom=op13_ui_log_bottom(m);
 if(top+72>bottom)top=bottom>72?bottom-72:0;
 return top;
}
static void op13_ui_logo(struct op13_canvas *c,const uint8_t *alpha,unsigned top)
{
 if(c->width<300||top>=c->height||300>c->height-top)return;
 unsigned left=(c->width-300)/2;
 for(unsigned y=0;y<300;y++)for(unsigned x=0;x<300;x++){
  unsigned a=alpha[y*300+x];if(!a)continue;
  uint32_t *p=&c->pixels[(size_t)(top+y)*c->stride+left+x],old=*p,value=0;
  for(unsigned sh=0;sh<24;sh+=8)value|=(a+(((old>>sh)&255)*(255-a)+127)/255)<<sh;
  *p=value;
 }
}
static void op13_ui_draw(struct op13_canvas *c,const struct op13_menu *m,const uint8_t *logo)
{
 const uint32_t bg=0x0b0c0f,text=0xf2f3f5,dim=0x7c838d,accent=0xef4444,rule=0x262a31;
 unsigned s=m->scale,fs=s>1?s-1:1,pad=8*s;
 op13_ui_rect(c,0,0,c->width,c->height,bg);
 op13_ui_text(c,m->left,m->logs_top+4*s,"UEFI BOOT MANAGER",fs,dim);
 unsigned logs_on=m->show_logs||m->focus==OP13_MENU_LOGS||(m->captured&&m->target==OP13_MENU_LOGS);
 op13_ui_rect(c,m->logs_left,m->logs_top,m->logs_width,m->logs_height,logs_on?accent:rule);
 op13_ui_rect(c,m->logs_left+2,m->logs_top+2,m->logs_width-4,m->logs_height-4,logs_on?0x3b1418:0x171a20);
 const char *toggle="Logs";
 op13_ui_text(c,m->logs_left+(m->logs_width-4*8*fs)/2,m->logs_top+(m->logs_height-19*fs)/2,toggle,fs,text);
 unsigned brand_y=128;
 if(op13_ui_logo_fits(m)){
  op13_ui_logo(c,logo,(c->height-300)/2);brand_y=op13_ui_brand_top(m);
 }
 if(op13_ui_logo_fits(m)||brand_y+19*(s+1)+24+s+32+19*fs+24<m->top){
 op13_ui_center(c,brand_y,"Project Boot",s+1,accent);
 unsigned line_y=brand_y+19*(s+1)+24;
 op13_ui_rect(c,(c->width-c->width/6)/2,line_y,c->width/6,s,accent);
 op13_ui_center(c,line_y+s+32,"OnePlus 13",fs,dim);
 }
 if(m->show_logs){
  unsigned lt=op13_ui_log_top(m),lb=op13_ui_log_bottom(m);
  op13_ui_rect(c,m->left,lt,m->width,lb-lt,rule);
  op13_ui_rect(c,m->left+2,lt+2,m->width-4,lb-lt-4,0);
 }
 for(unsigned action=OP13_MENU_IOS;action<=OP13_MENU_ANDROID;action++){
  unsigned top=m->top+(action-1)*(m->height+m->gap),enabled=!m->uploading&&(action==OP13_MENU_ANDROID||m->ios_ready);
  unsigned selected=m->focus==action||(m->captured&&m->target==action);
  uint32_t row=selected?0x3b1418:0x171a20;
  op13_ui_rect(c,m->left,top,m->width,m->height,selected?accent:rule);
  op13_ui_rect(c,m->left+2,top+2,m->width-4,m->height-4,row);
  if(selected)op13_ui_rect(c,m->left,top,2*s,m->height,text);
  unsigned badge=24*s,bx=m->left+pad,by=top+(m->height-badge)/2;
  op13_ui_rect(c,bx,by,badge,badge,selected?0x7f1d1d:rule);
  op13_ui_text(c,bx+(badge-16*fs)/2,by+(badge-19*fs)/2,action==OP13_MENU_IOS?"01":"02",fs,enabled?text:dim);
  unsigned tx=bx+badge+pad;
  op13_ui_text(c,tx,top+pad,action==OP13_MENU_IOS?"Boot iOS":"Return to Android",s,enabled?text:dim);
  const char *hint=m->uploading?"Available after upload":action==OP13_MENU_ANDROID?"Restart your phone":(enabled?"Ready to start":"Waiting for iOS files");
  op13_ui_text(c,tx,top+m->height-pad-19*fs,hint,fs,dim);
 }
 unsigned bottom=m->top+2*m->height+m->gap;
 op13_ui_rect(c,m->left,bottom+6*s,m->width,s>1?s/2:1,rule);
 op13_ui_center(c,bottom+18*s,(!m->visible||m->uploading)?"Preparing iOS in RAM...":(m->ios_ready?"iOS ready in RAM":"Connect to your Mac to prepare iOS"),fs,dim);
 unsigned hs=s>2?s-2:1;
 op13_ui_center(c,c->height-40*s,m->show_logs?"Drag down for older logs; up for latest":(m->uploading?"Tap Logs or use Volume + Power":"Tap an option or use Volume + Power"),hs,dim);
 op13_ui_center(c,c->height-24*s,"Temporary boot - RAM only",hs,dim);
}
static uint32_t op13_ui_compose(const struct op13_canvas *ui,const struct op13_menu *m,const uint8_t *logo,unsigned x,unsigned y,uint32_t logs)
{
 uint32_t pixel=ui->pixels[(size_t)y*ui->stride+x];
 /* Input visibility is suspended during uploads; the cached UI stays visible. */
 if(m->screen_width&&!m->show_logs)return pixel;
 /* Confine all console pixels to the box above the centered logo. */
 if(x<m->left+16||x>=m->left+m->width-16||
    y<op13_ui_log_top(m)+16||y>=op13_ui_log_bottom(m)-16)return pixel;
 return logs;
}
#endif
