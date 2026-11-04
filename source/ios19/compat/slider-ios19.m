#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <sys/sysctl.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
static void note(const char *fmt,...) {
 char buf[768];va_list ap;va_start(ap,fmt);int n=vsnprintf(buf,sizeof(buf),fmt,ap);va_end(ap);
 int fd=open("/dev/console",O_WRONLY);if(fd>=0){write(fd,buf,MIN(MAX(n,0),sizeof(buf)-1));close(fd);}
}
static BOOL api(Class cls,SEL sel,const char *ret,unsigned argc,const char **args,BOOL meta) {
 Method m=meta?class_getClassMethod(cls,sel):class_getInstanceMethod(cls,sel);char type[128]={0};
 if(!m||method_getNumberOfArguments(m)!=argc)return NO;
 method_getReturnType(m,type,sizeof(type));if(strcmp(type,ret))return NO;
 for(unsigned i=2;i<argc;i++){method_getArgumentType(m,i,type,sizeof(type));if(args && args[i-2] && strcmp(type,args[i-2]))return NO;}
 return YES;
}
static unsigned position(void){unsigned p=0;size_t n=sizeof(p);return sysctlbyname("kern.redwood.slider",&p,&n,NULL,0)==0?p:0;}
static dispatch_queue_t focusQueue;
static void focus(BOOL top) {
 static id service;static int held=-1;
 if(held==(int)top)return;
 if(!service) {
  dlopen("/System/Library/PrivateFrameworks/DoNotDisturb.framework/DoNotDisturb",RTLD_NOW|RTLD_LOCAL);
  Class cls=objc_getClass("DNDModeAssertionService");SEL make=sel_registerName("serviceForClientIdentifier:");
  const char *arg[]={"@"};if(!api(cls,make,"@",3,arg,YES)){note("slider19: Focus service API unsupported\n");return;}
  service=((id(*)(id,SEL,id))objc_msgSend)(cls,make,@"com.apple.springboard.donotdisturb.hid");
 }
 if(!service)return;
 NSError *error=nil;
 if(top) {
  Class cls=objc_getClass("DNDModeAssertionDetails");SEL make=sel_registerName("userRequestedAssertionDetailsWithIdentifier:modeIdentifier:lifetime:");const char *args[]={"@","@","@"};
  if(!api(cls,make,"@",5,args,YES)){note("slider19: Focus assertion API unsupported\n");return;}
  id details=((id(*)(id,SEL,id,id,id))objc_msgSend)(cls,make,@"com.apple.springboard.donotdisturb.hid",@"com.apple.donotdisturb.mode.default",nil);
  SEL take=sel_registerName("takeModeAssertionWithDetails:error:");const char *takeArgs[]={"@","^@"};
  if(!api(object_getClass(service),take,"@",4,takeArgs,NO))return;
  id assertion=((id(*)(id,SEL,id,NSError *__autoreleasing *))objc_msgSend)(service,take,details,&error);
  note("slider19: Do Not Disturb take=%d error=%s\n",assertion!=nil,error.description.UTF8String ?: "none");if(assertion)held=1;
 } else {
  SEL release=sel_registerName("invalidateAllActiveModeAssertionsWithError:");const char *args[]={"^@"};
  if(!api(object_getClass(service),release,"B",3,args,NO))return;
  BOOL ok=((BOOL(*)(id,SEL,NSError *__autoreleasing *))objc_msgSend)(service,release,&error);
  note("slider19: release slider Focus assertion=%d error=%s\n",ok,error.description.UTF8String ?: "none");if(ok)held=0;
 }
}
static BOOL ringer(unsigned pos,BOOL feedback) {
 id app=UIApplication.sharedApplication;SEL getter=sel_registerName("ringerControl");
 if(!api(object_getClass(app),getter,"@",2,NULL,NO))return NO;
 id control=((id(*)(id,SEL))objc_msgSend)(app,getter);if(!control)return NO;
 SEL set=sel_registerName("setRingerMuted:withFeedback:reason:clientType:");const char *args[]={"B","B","@","I"};
 if(!api(object_getClass(control),set,"v",6,args,NO)) {
  static BOOL warned;if(!warned){warned=YES;note("slider19: native ringer ABI unsupported\n");}return NO;
 }
 ((void(*)(id,SEL,BOOL,BOOL,id,unsigned int))objc_msgSend)(control,set,pos==2,feedback,@"org.ios-dodge.alert-slider",0);
 note("slider19: native ringer position=%u silent=%d feedback=%d\n",pos,pos==2,feedback);return YES;
}
__attribute__((constructor)) static void start(void) {
 if(strcmp(getprogname(),"SpringBoard"))return;
 focusQueue=dispatch_queue_create("org.ios-dodge.slider.focus",DISPATCH_QUEUE_SERIAL);
 static dispatch_source_t timer;timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_main_queue());
 dispatch_source_set_timer(timer,dispatch_time(DISPATCH_TIME_NOW,20LL*NSEC_PER_SEC),200*NSEC_PER_MSEC,25*NSEC_PER_MSEC);
 dispatch_source_set_event_handler(timer,^{@autoreleasepool {
  static unsigned previous=0;unsigned current=position();if(current<1||current>3||current==previous)return;
  if(!ringer(current,previous!=0))return;
  previous=current;
  dispatch_async(focusQueue,^{@autoreleasepool{unsigned latest=position();if(latest>=1&&latest<=3)focus(latest==3);}});
 }});
 dispatch_resume(timer);note("slider19: bottom Ring, middle Silent, top DND; 200ms sleeping poll\n");
}
