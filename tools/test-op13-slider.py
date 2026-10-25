#!/usr/bin/env python3
"""Exercise the actual open-drain transport with a scripted ACK/data sensor."""
from pathlib import Path
import argparse
import subprocess
import tempfile
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from op13_fdt import read_fdt,cells
r=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--dtb',type=Path,help='Optional local Android FDT to verify the hardware profile')
args=parser.parse_args()
if args.dtb is not None:
 n=read_fdt(args.dtb.read_bytes())
 hall=n['/soc/qcom,qupv3_i2c_geni_se@9c0000/i2c@9a4000/magnachip@10']
 assert hall['compatible']==b'oplus,dhall-ak09970\0' and cells(hall['reg'])==[0x10]
 for pin,name in [(80,'sda'),(81,'scl')]:
  p=f'/soc/pinctrl@f000000/qupv3_hub_i2c9_pins/qupv3_hub_i2c9_{name}_active/mux'
  assert n[p]['pins']==f'gpio{pin}\0'.encode()
  assert 0xf000000+0x100000+pin*0x1000 in [0xf150000,0xf151000]
 print('PASS: sensor address and GPIOs match the supplied phone DT')
assert (r/'source/touch-runtime-v15/op13_slider_i2c.h').read_bytes()==(r/'source/xnu-build-snapshot/xnu-8796.121.2/bsd/sys/op13_slider_i2c.h').read_bytes()
code=r'''
#include <assert.h>
#include <stdio.h>
#include "op13_slider_i2c.h"
#include "op13_slider_position.h"
struct mock {uint32_t cfg[2],data[2];unsigned started,pos,count,delays,stretch;int bits[64];};
static uint32_t rd(void *c,unsigned p,unsigned o) {
 struct mock *m=c;assert(p<2&&(o==0||o==4));
 if(!o)return m->cfg[p];
 if(!m->started)return m->data[p];
 if(p==1)return m->stretch?0:1;
 assert(m->pos<m->count);return m->bits[m->pos++];
}
static void wr(void *c,unsigned p,unsigned o,uint32_t v) {
 struct mock *m=c;assert(p<2&&(o==0||o==4));m->started=1;
 if(!o)m->cfg[p]=v;else m->data[p]=v;
 assert(!(m->cfg[p]&0x200)||!(m->data[p]&2)); /* never drive high */
}
static void pause_us(void *c,unsigned us) {struct mock *m=c;assert(us<=4);m->delays+=us;}
static void fresh(struct mock *m){*m=(struct mock){.cfg={0x1004,0x1004},.data={3,3}};}
static void bit(struct mock *m,int v){m->bits[m->count++]=v;}
static void byte(struct mock *m,uint8_t b){for(int i=7;i>=0;i--)bit(m,(b>>i)&1);}
static void restored(struct mock *m){assert(m->cfg[0]==0x1004&&m->cfg[1]==0x1004&&m->data[0]==3&&m->data[1]==3);}
int main(void) {
 const int cal[]={6500,11500,-2100,300,16800,-2700,-6500,12000,-2300};
 assert(op13_slider_calibration_valid(cal));
 for(unsigned i=0;i<3;i++)assert(op13_slider_position(cal,cal[3*i],cal[3*i+1],cal[3*i+2])==3-i);
 assert(op13_slider_position(cal,3400,14000,-2300)==0);
 assert(op13_slider_position(cal,0,30000,-2300)==0);
 assert(op13_slider_position(cal,6500,11500,8000)==0);
 int mirrored[9];for(unsigned i=0;i<9;i++)mirrored[i]=(i%3)?cal[i]:-cal[i];
 for(unsigned i=0;i<3;i++)assert(op13_slider_position(mirrored,mirrored[3*i],mirrored[3*i+1],mirrored[3*i+2])==3-i);
 int invalid[9]={0};assert(!op13_slider_calibration_valid(invalid));

 struct mock m;fresh(&m);
 struct op13_slider_bus b={.cookie=&m,.read=rd,.write=wr,.delay=pause_us};
 bit(&m,1);bit(&m,0);bit(&m,0);bit(&m,1);bit(&m,0);byte(&m,0x48);byte(&m,0xc1);
 uint8_t id[2]={0};assert(!op13_slider_transfer(&b,0,id,2,0));
 assert(id[0]==0x48&&id[1]==0xc1&&m.pos==m.count);restored(&m);
 fresh(&m);bit(&m,1);bit(&m,1);
 assert(op13_slider_transfer(&b,0,id,2,0)==-1);restored(&m);
 fresh(&m);m.stretch=1;
 assert(op13_slider_transfer(&b,0,id,2,0)==-2&&m.delays<2100);restored(&m);
 fresh(&m);bit(&m,0);
 assert(op13_slider_transfer(&b,0,id,2,0)==-3);restored(&m);
 fresh(&m);assert(op13_slider_transfer(&b,0,id,9,0)==-4&&!m.started);
 puts("PASS: calibration/position classification; ACK/data, NACK, stuck SCL/SDA, bounds and mux restoration; open drain only");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(code)
 subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-iquote',str(r/'source/touch-runtime-v15'),'-iquote',str(r/'source/xnu-build-snapshot/xnu-8796.121.2/bsd/sys'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
