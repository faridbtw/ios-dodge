/* Scope brightness availability to the working screen bridge, not all of BKS.
 * Keep the original SpringBoard library byte-for-byte: load it first, then
 * install this one method after DisplayModule is loaded by Control Center.
 */
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern const void *_dyld_get_shared_cache_range(size_t *);
static BOOL (*original_available)(id,SEL);
static BOOL screen_available(id self,SEL selector)
{
 return access("/dev/op13-screen",F_OK)==0?YES:original_available(self,selector);
}
static dispatch_source_t timer;
static void install(void)
{
 Class cls=objc_getClass("CCUIDisplayModuleViewController");
 if(!cls)return;
 Method m=class_getInstanceMethod(cls,sel_registerName("_canAdjustBrightness"));
 size_t length=0;uintptr_t base=(uintptr_t)_dyld_get_shared_cache_range(&length);
 if(!m||!base)return;
 IMP imp=method_getImplementation(m);
 if((uintptr_t)imp!=0x20915f548ULL+base-0x180000000ULL){
  fprintf(stderr,"op13-screen-cc: unexpected availability method; left unchanged\n");
  dispatch_source_cancel(timer);return;
 }
 char *type=method_copyReturnType(m);
 int valid=type&&(type[0]=='B'||type[0]=='c')&&method_getNumberOfArguments(m)==2;
 free(type);
 if(!valid){dispatch_source_cancel(timer);return;}
 original_available=(BOOL (*)(id,SEL))imp;
 method_setImplementation(m,(IMP)screen_available);
 fprintf(stderr,"op13-screen-cc: scoped Control Center availability installed\n");
 dispatch_source_cancel(timer);
}
__attribute__((constructor)) static void begin(void)
{
 if(!dlopen("/redwood/lib/libredwood-sb.dylib",RTLD_NOW|RTLD_GLOBAL)){
  fprintf(stderr,"op13-screen-cc: original SpringBoard hooks unavailable: %s\n",dlerror());return;
 }
 if(access("/dev/op13-screen",F_OK))return;
 timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_main_queue());
 if(!timer)return;
 dispatch_source_set_timer(timer,dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),NSEC_PER_SEC,100*NSEC_PER_MSEC);
 dispatch_source_set_event_handler(timer,^{install();});
 dispatch_resume(timer);
}
