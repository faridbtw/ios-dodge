/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef OP13_SLIDER_POSITION_H
#define OP13_SLIDER_POSITION_H
static int op13_slider_abs(int a){return a<0?-a:a;}
static int op13_slider_calibration_valid(const int *c)
{
 for(unsigned i=0;i<9;i++)if(c[i]<-32768||c[i]>32767)return 0;
 int a=c[0]-c[3],b=c[3]-c[6];
 return op13_slider_abs(a)>=2000&&op13_slider_abs(b)>=2000&&((a>0)==(b>0));
}
static unsigned op13_slider_position(const int *c,int x,int y,int z)
{
 /* Vendor V2: middle/end X regions at 60% of calibrated spacing.
  * Reject transition gaps and strong external-field interference. */
 if(!op13_slider_calibration_valid(c))return 0;
 int width_up=op13_slider_abs(c[0]-c[3])*6/10;
 int width_down=op13_slider_abs(c[3]-c[6])*6/10;
 int sign=c[0]>c[6]?1:-1;
 int v=x*sign,up=c[0]*sign,mid=c[3]*sign,down=c[6]*sign;
 unsigned index;
 if(v>mid+width_up&&v<up+width_up)index=0;
 else if(v>down+width_down&&v<up-width_up)index=1;
 else if(v>down-width_down&&v<mid-width_down)index=2;
 else return 0;
 if(op13_slider_abs(z-c[index*3+2])>7000||op13_slider_abs(y-c[index*3+1])>7000)return 0;
 static const unsigned positions[]={3,2,1};
 return positions[index];
}
#endif
