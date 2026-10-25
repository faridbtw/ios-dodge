#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static struct panel { unsigned pins_owned,power; } identify_panel;
static struct session { unsigned panel_owned,blocked; } identify_session;
static struct bus { unsigned owned; } bb_state;
static uint32_t reset_io;
static unsigned calls,fail_at,fail_bus,fail_panel,bus_calls,panel_calls;
static int sm8750_panel_raw_read(void *c,uint32_t o,uint32_t *v)
{ (void)c;assert(o==0xa1004);if(++calls==fail_at)return -1;*v=reset_io;return 0; }
static int sm8750_panel_raw_write(void *c,uint32_t o,uint32_t v)
{ (void)c;assert(o==0xa1004);reset_io=v;return ++calls==fail_at?-1:0; }
static int op13_bitbang_cleanup(struct bus *b)
{
 bus_calls++;assert(!(reset_io&2)&&identify_panel.power);
 if(fail_bus)return -1;
 b->owned=0;return 0;
}
static int sm8750_panel_release(struct panel *p)
{
 panel_calls++;assert(!bb_state.owned&&!(reset_io&2));
 if(fail_panel)return -1;
 p->power=p->pins_owned=0;return 0;
}
#include "bitbang_cleanup.inc"
static void fresh(void)
{
 identify_panel=(struct panel){1,1};identify_session=(struct session){1,1};
 bb_state=(struct bus){1};reset_io=3;
 calls=fail_at=fail_bus=fail_panel=bus_calls=panel_calls=0;
}
int main(void)
{
 fresh();assert(!bb_cleanup_all());
 assert(!identify_panel.power&&!bb_state.owned&&!identify_session.blocked);
 for(unsigned n=1;n<=3;n++){
  fresh();fail_at=n;assert(bb_cleanup_all()==-1);
  assert(!bus_calls&&!panel_calls&&bb_state.owned&&identify_panel.power);
  assert(identify_session.blocked&&identify_session.panel_owned);
  fail_at=0;assert(!bb_cleanup_all());
 }
 fresh();fail_bus=1;assert(bb_cleanup_all()==-1);
 assert(!panel_calls&&identify_panel.power&&identify_panel.pins_owned&&bb_state.owned);
 fail_bus=0;assert(!bb_cleanup_all()&&panel_calls==1);
 fresh();fail_panel=1;assert(bb_cleanup_all()==-1);
 assert(!bb_state.owned&&identify_panel.power&&identify_session.blocked);
 fail_panel=0;assert(!bb_cleanup_all()&&!identify_session.blocked);
 puts("PASS actual cleanup helper: reset-before-pins-before-power; reset/read/landed-write errors, retained SPI/power and retry");
}
