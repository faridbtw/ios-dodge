#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
static void note(const char *fmt,...) {char b[1600];va_list a;va_start(a,fmt);int n=vsnprintf(b,sizeof b,fmt,a);va_end(a);int fd=open("/dev/console",O_WRONLY);if(fd>=0){write(fd,b,MIN(MAX(n,0),sizeof(b)-1));close(fd);}}
static CGSize (*nativeSize)(id,SEL);
static NSDirectionalEdgeInsets (*nativeInsets)(id,SEL);
static BOOL phone(void){CGSize s=UIScreen.mainScreen.bounds.size;return fabs(s.width-375)<1 && fabs(s.height-812)<1;}
static CGSize sensorSize(id obj,SEL sel){CGSize v=nativeSize(obj,sel);static BOOL once;if(!once){once=YES;note("status-layout: native sensor %.2fx%.2f\n",v.width,v.height);}if(phone())v.height=30;return v;}
static NSDirectionalEdgeInsets sensorInsets(id obj,SEL sel){NSDirectionalEdgeInsets v=nativeInsets(obj,sel);static BOOL once;if(!once){once=YES;note("status-layout: native island insets %.2f %.2f %.2f %.2f\n",v.top,v.leading,v.bottom,v.trailing);}if(phone())v.top=4;return v;}
static void tree(UIView *v,int depth,BOOL active){if(depth>16)return;const char *c=object_getClassName(v);BOOL here=active||strstr(c,"StatusBar")||strstr(c,"Aperture");if(here){CGRect r=[v convertRect:v.bounds toView:nil];note("status-layout: view %s frame %.1f %.1f %.1f %.1f hidden %d alpha %.2f sub %lu\n",c,r.origin.x,r.origin.y,r.size.width,r.size.height,v.hidden,v.alpha,(unsigned long)v.subviews.count);if(strstr(c,"StringView")||strstr(c,"Label")){SEL s=sel_registerName("text");if([v respondsToSelector:s]){id t=((id(*)(id,SEL))objc_msgSend)(v,s);if([t isKindOfClass:NSString.class]&&[t length]<20)note("status-layout: label %s text=%s\n",c,[t UTF8String]);}}}for(UIView *s in v.subviews)tree(s,depth+1,here);}
static BOOL replace(Class c,const char *name,const char *ret,IMP imp,IMP *old){Method m=class_getInstanceMethod(c,sel_registerName(name));char t[160]={0};if(m)method_getReturnType(m,t,sizeof t);if(!m||strcmp(t,ret)||method_getNumberOfArguments(m)!=2){note("status-layout: unsupported %s %s type=%s\n",class_getName(c),name,t);return NO;}*old=method_setImplementation(m,imp);return YES;}
// Redirect only the new SpringBoard wrapper's allocation. Existing UIKit
// initialization/data/legibility/animation methods stay native.
static id statusAlloc(id cls,SEL sel,NSZone *zone) __attribute__((ns_returns_retained));
static id statusAlloc(id cls,SEL sel,NSZone *zone) {
 static BOOL once;if(!once){once=YES;note("status-layout: allocating native UIKit status bar for SpringBoard\n");}
 return [objc_getClass("UIStatusBar_Modern") allocWithZone:zone];
}
static CGRect (*nativeSensorRect)(id,SEL);
static CGRect statusSensorRect(id obj,SEL sel) {
 CGRect r=nativeSensorRect(obj,sel);
 if(phone())r=CGRectMake((375.0-125.0)/2.0,4,125,30);
 return r;
}
static void nativeStatusFallback(void) {
 dlopen("/System/Library/PrivateFrameworks/SystemStatusUI.framework/SystemStatusUI",RTLD_LAZY|RTLD_LOCAL);
 Class cls=objc_getClass("STUIStatusBar_Wrapper");
 Class legacy=objc_getClass("UIStatusBar_Modern");
 Method m=class_getClassMethod(cls,@selector(allocWithZone:));
 char type[32]={0};if(m)method_getReturnType(m,type,sizeof type);
 if(cls && legacy && [legacy isSubclassOfClass:objc_getClass("UIStatusBar_Base")] && m && !strcmp(type,"@") && method_getNumberOfArguments(m)==3) {
  Class meta=object_getClass(cls);
  if(!class_addMethod(meta,@selector(allocWithZone:),(IMP)statusAlloc,method_getTypeEncoding(m))) {
   Method own=class_getClassMethod(cls,@selector(allocWithZone:));method_setImplementation(own,(IMP)statusAlloc);
  }
  note("status-layout: native status bar allocation compatibility installed\n");
 } else note("status-layout: allocation API unsupported type=%s\n",type);
 replace(objc_getClass("_UIStatusBarVisualProvider_DynamicSplit"),"sensorAreaRect",@encode(CGRect),(IMP)statusSensorRect,(IMP*)&nativeSensorRect);
}
static void (*nativeBatteryLayout)(id,SEL);
static void batteryLayout(UIView *view,SEL sel) {
 if(phone())view.transform=CGAffineTransformIdentity;
 nativeBatteryLayout(view,sel);
 if(!phone() || !view.window)return;
 CGRect r=[view convertRect:view.bounds toView:view.window];
 if(r.size.width<10 || r.size.width>40 || r.origin.y>54)return;
 CGFloat shift=375.0-22.0-CGRectGetMaxX(r);
 view.transform=CGAffineTransformMakeTranslation(shift,0);
 static BOOL once;if(!once){once=YES;note("status-layout: battery native x=%.2f width=%.2f shift=%.2f right-inset=22\n",r.origin.x,r.size.width,shift);}
}
static BOOL noarg(id obj,const char *name) {
 Method m=class_getInstanceMethod(object_getClass(obj),sel_registerName(name));char t[32]={0};if(m)method_getReturnType(m,t,sizeof t);
 if(!m || strcmp(t,"v") || method_getNumberOfArguments(m)!=2)return NO;
 ((void(*)(id,SEL))objc_msgSend)(obj,sel_registerName(name));return YES;
}
static void restartClock(void) {
 id app=UIApplication.sharedApplication;SEL get=sel_registerName("statusBarStateAggregator");
 Method m=class_getInstanceMethod(object_getClass(app),get);char t[32]={0};if(m)method_getReturnType(m,t,sizeof t);
 if(!m || t[0]!='@' || method_getNumberOfArguments(m)!=2)return;
 id agg=((id(*)(id,SEL))objc_msgSend)(app,get);
 if(!agg)return;
 BOOL ok=noarg(agg,"_restartTimeItemTimer");
 note("status-layout: native clock timer restart %d aggregator=%s\n",ok,object_getClassName(agg));
 const char *fields[]={"_timeItemTimeString", "_shortTimeItemTimeString", "_timeItemDateFormatter", "_dateTimeProvider"};
 for(unsigned i=0;i<4;i++) { const char *name=fields[i];
  Ivar iv=class_getInstanceVariable(object_getClass(agg),name);
  if(iv && ivar_getTypeEncoding(iv)[0]=='@') {
   id value=object_getIvar(agg,iv);
   note("status-layout: clock field %s=%s\n",name,[[value description] UTF8String] ?: "nil");
  }
 }
 Ivar data=class_getInstanceVariable(object_getClass(agg),"_data");
 if(data && strstr(ivar_getTypeEncoding(data),"[46B]\"timeString\"[64c]") &&
    class_getInstanceSize(object_getClass(agg)) >= (size_t)ivar_getOffset(data)+174) {
  const unsigned char *bytes=(const unsigned char *)(__bridge void *)agg+ivar_getOffset(data);
  note("status-layout: clock native enabled=%u text=%.*s short=%.*s\n",bytes[0],64,bytes+46,64,bytes+110);
 }

}
__attribute__((constructor)) static void start(void){if(strcmp(getprogname(),"SpringBoard"))return;nativeStatusFallback();replace(objc_getClass("_UIStatusBarBatteryView"),"layoutSubviews","v",(IMP)batteryLayout,(IMP*)&nativeBatteryLayout);dispatch_after(dispatch_time(DISPATCH_TIME_NOW,30LL*NSEC_PER_SEC),dispatch_get_main_queue(),^{restartClock();});Class m=objc_getClass("SBUISystemApertureLayoutMetrics");replace(m,"sensorRegionSize",@encode(CGSize),(IMP)sensorSize,(IMP*)&nativeSize);replace(m,"minimumScreenEdgeInsets",@encode(NSDirectionalEdgeInsets),(IMP)sensorInsets,(IMP*)&nativeInsets);note("status-layout: loaded camera alignment candidate; only 375x812 portrait\n");for(int sec=30;sec<=150;sec+=30)dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)sec*NSEC_PER_SEC),dispatch_get_main_queue(),^{note("status-layout: snapshot seconds=%d bounds=%s\n",sec,NSStringFromCGRect(UIScreen.mainScreen.bounds).UTF8String);for(UIScene *s in UIApplication.sharedApplication.connectedScenes)if([s isKindOfClass:UIWindowScene.class])for(UIWindow *w in ((UIWindowScene*)s).windows)tree(w,0,NO);});}
