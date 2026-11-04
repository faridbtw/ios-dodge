#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
static void queueLog(const char *fmt,...) {
 char buf[512];va_list args;va_start(args,fmt);int n=vsnprintf(buf,sizeof buf,fmt,args);va_end(args);
 if(n>0){int fd=open("/dev/console",O_WRONLY);if(fd>=0){write(fd,buf,MIN((size_t)n,sizeof(buf)-1));close(fd);}}
}
#define fprintf(stream,...) queueLog(__VA_ARGS__)

// iOS 27 PosterBoard snapshot queue. The native kickoff method holds its own
// manager lock while consulting each provider's capacity. Keep its queues,
// timeouts, callbacks and invalidation intact; reduce global preview concurrency.
static void (*originalKickoff)(id,SEL);
static BOOL (*originalCapacity)(id,SEL,NSUInteger);
static __thread void *currentManager;
static Ivar activeRequests;
static void (*originalRelease)(id,SEL,id,BOOL);
static _Atomic unsigned deferrals;

static void kickoff(id self,SEL selector) {
    void *previous=currentManager;
    currentManager=(__bridge void *)self;
    @try { originalKickoff(self,selector); }
    @finally { currentManager=previous; }
}
static void releasePreview(id self,SEL selector,id snapshotter,BOOL terminate) {
    // Use the native termination path once the snapshot is consumed, so idle
    // preview processes do not accumulate in this RAM-root configuration.
    originalRelease(self,selector,snapshotter,YES);
}
static BOOL capacity(id self,SEL selector,NSUInteger requested) {
    if(currentManager) {
        // Called by the native kickoff under the manager's lock. Retained idle
        // snapshotter instances do not represent active work and must not block it.
        id requests=object_getIvar((__bridge id)currentManager,activeRequests);
        NSUInteger running=[requests count];
        if(running>=1) {
            unsigned count=atomic_fetch_add(&deferrals,1)+1;
            if(count<=8 || count%100==0)
                fprintf(stderr,"wallpaper-queue: defer preview; native running=%lu deferrals=%u\n",(unsigned long)running,count);
            return NO;
        }
        requested=MIN(requested,(NSUInteger)1);
    }
    return originalCapacity(self,selector,requested);
}
__attribute__((constructor)) static void install(void) {
    if(strcmp(getprogname(),"PosterBoard"))return;
    Class manager=objc_getClass("PBFPosterSnapshotManager");
    Class provider=objc_getClass("PBFPosterSnapshotProviderTracker");
    Method kick=class_getInstanceMethod(manager,sel_registerName("_lock_kickoffNextOperation"));
    Method cap=class_getInstanceMethod(provider,sel_registerName("canAcceptMoreSnapshotters:"));
    activeRequests=class_getInstanceVariable(manager,"_lock_activePUIRequests");
    Method release=class_getInstanceMethod(provider,sel_registerName("releaseSnapshotter:shouldTerminate:"));
    if(!manager || !provider || !kick || !cap || !activeRequests || !release) {
        fprintf(stderr,"wallpaper-queue: incompatible runtime; no hooks installed\n");return;
    }
    char kr[16]={0},cr[16]={0},arg[16]={0};
    method_getReturnType(kick,kr,sizeof kr);method_getReturnType(cap,cr,sizeof cr);
    method_getArgumentType(cap,2,arg,sizeof arg);
    ptrdiff_t activeOffset=ivar_getOffset(activeRequests);
    char rr[16]={0},ro[32]={0},rb[16]={0};
    method_getReturnType(release,rr,sizeof rr);method_getArgumentType(release,2,ro,sizeof ro);method_getArgumentType(release,3,rb,sizeof rb);
    if(strcmp(kr,"v") || (strcmp(cr,"B") && strcmp(cr,"c")) || strcmp(arg,"Q") ||
       ivar_getTypeEncoding(activeRequests)[0]!='@' || method_getNumberOfArguments(kick)!=2 ||
       method_getNumberOfArguments(cap)!=3 || activeOffset<0 ||
       (size_t)activeOffset+sizeof(id)>class_getInstanceSize(manager) ||
       strcmp(rr,"v") || ro[0]!='@' || (strcmp(rb,"B") && strcmp(rb,"c")) || method_getNumberOfArguments(release)!=4) {
        fprintf(stderr,"wallpaper-queue: ABI check failed; no hooks installed\n");return;
    }
    originalRelease=(void (*)(id,SEL,id,BOOL))method_setImplementation(release,(IMP)releasePreview);
    originalCapacity=(BOOL (*)(id,SEL,NSUInteger))method_setImplementation(cap,(IMP)capacity);
    originalKickoff=(void (*)(id,SEL))method_setImplementation(kick,(IMP)kickoff);
    fprintf(stderr,"wallpaper-queue: installed active preview limit=1, retire idle previews (ivar offset=%td)\n",activeOffset);
}
