from pathlib import Path
here=Path(__file__).resolve().parent
p=here/'loader.c'
s=p.read_text()
def replace(old,new):
    global s
    assert s.count(old)==1, old[:120]
    s=s.replace(old,new)
replace('static int starts_with(const char *s, const char *prefix);\n',
'''static int starts_with(const char *s, const char *prefix);
static unsigned touch_runtime_pending,touch_runtime_outstanding;
static uint64_t touch_runtime_lease_address(void);
static int touch_runtime_accept(const char *);
static void touch_runtime_after_return(void);
''')
replace('\tif (!have_checkpoint) {\n\t\tcheckpoint = pi.checkpoint;',
'''\tif (touch_runtime_pending && !touch_runtime_accept(pi.cmdline)) {
        print("touch-runtime refused incompatible payload arguments\\n");return 0;
    }
\tif (!have_checkpoint) {
\t\tcheckpoint = pi.checkpoint;''')
replace('\t/* --- boot_args ------------------------------------------------------ */',
'''    if(touch_runtime_pending && kernel_mpidr==this_mpidr){
        print("touch-runtime requires resident loader on a separate core\\n");return 0;
    }
\t/* --- boot_args ------------------------------------------------------ */''')
replace('\tfor (const char *c = pi.cmdline; *c && n < BOOT_LINE_LENGTH - 1; c++) {',
'''    if(touch_runtime_pending){
        const char *prefix="touch_lease_pa=0x",*hex="0123456789abcdef";
        uint64_t address=touch_runtime_lease_address();
        for(const char *c=prefix;*c&&n<BOOT_LINE_LENGTH-1;c++)ba->commandLine[n++]=*c;
        for(int shift=60;shift>=0&&n<BOOT_LINE_LENGTH-1;shift-=4)
            ba->commandLine[n++]=hex[(address>>shift)&15];
        if(n<BOOT_LINE_LENGTH-1)ba->commandLine[n++]=' ';
    }
\tfor (const char *c = pi.cmdline; *c && n < BOOT_LINE_LENGTH - 1; c++) {''')
replace('\t\t\tint64_t status = run_on_other_cpu(kernel_mpidr, entry_va + slide, ba_pa, kernel_live, slide, kernel_budget);',
'''            touch_runtime_outstanding=touch_runtime_pending;
\t\t\tint64_t status = run_on_other_cpu(kernel_mpidr, entry_va + slide, ba_pa, kernel_live, slide, kernel_budget);
            if(status==0){touch_runtime_outstanding=0;touch_runtime_after_return();}''')
replace('#include "bitbang_diag.inc"','#include "bitbang_diag.inc"\n#include "touch_runtime.inc"')
replace('int busy = running_live != NULL;',
'''int busy = running_live != NULL;
 if(touch_runtime_pending && !str_eq(cmd,"oem sm8750-touch-status") &&
    !str_eq(cmd,"oem sm8750-touch-cleanup") && !str_eq(cmd,"oem log") &&
    !str_eq(cmd,"upload") && !str_eq(cmd,"oem stop") &&
    !(starts_with(cmd,"oem run")&&!touch_runtime_outstanding&&!identify_session.blocked) &&
    !starts_with(cmd,"getvar:")){
  fb_reply("FAIL","touch lease active; only run, stop, status, logs or cleanup");return;
 }''')
replace('(identify_session.handle || identify_session.panel_owned || identify_session.blocked) &&',
        '(identify_session.handle || identify_session.panel_owned || identify_session.blocked || bb_state.owned) && !touch_runtime_pending &&')
replace('  if(busy){fb_reply("FAIL","kernel running");return;}\n  int e=bb_cleanup_all();',
'''  if(busy||touch_runtime_outstanding){fb_reply("FAIL","kernel still owns touch lease");return;}
  int e=bb_cleanup_all();
  if(!e)touch_runtime_clear();''')
replace(' if (str_eq(cmd,"oem sm8750-touch-status")) {',
''' if(str_eq(cmd,"oem sm8750-touch-arm")){
  if(busy){fb_reply("FAIL","kernel running");return;}
  touch_trace_stream=1;int e=touch_runtime_arm();touch_trace_stream=0;
  fb_reply(e?"FAIL":"OKAY",e?"touch arm failed; inspect log":"GPIO lease armed for XNU");return;
 }
 if (str_eq(cmd,"oem sm8750-touch-status")) {
  print("touch-runtime pending %d outstanding %d lease %x\\n",(uint64_t)touch_runtime_pending,(uint64_t)touch_runtime_outstanding,touch_runtime_lease_address());''')
p.write_text(s,newline='\n')
# Embed the exact tested transport in the XNU board compilation unit.
header=(here/'op13_bitbang.h').read_text()
transport=(here/'op13_bitbang.c').read_text().replace('#include "op13_bitbang.h"',header)
(here/'redwood_s3910_board_gpio.c').write_text(transport+'\n'+(here/'xnu_board_tail.c').read_text(),newline='\n')
# Preserve worker semantics, expose all setup/errors and first32 events to live USB log.
worker=(here.parent.parent/'touch/src/redwood_s3910.c').read_text()
anchor='extern int sm8750_touch_bus_open(struct s3910_io *);'
say='''extern void loader_log_putc(char);
static int touch_say(const char *format,...) __printflike(1,2);
static int touch_say(const char *format,...)
{
 char line[400];va_list ap;va_start(ap,format);
 vsnprintf(line,sizeof(line),format,ap);va_end(ap);
 for(const char *c=line;*c;c++)loader_log_putc(*c);
 return printf("%s",line);
}
#define printf touch_say
'''
assert worker.count(anchor)==1
worker=worker.replace(anchor,say+'\n'+anchor)
(here/'redwood_s3910_gpio_worker.c').write_text(worker,newline='\n')
print('Integrated immutable loader GPIO lease and generated XNU board/worker sources')
