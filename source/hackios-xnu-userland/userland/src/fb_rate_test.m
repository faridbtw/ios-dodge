/* Distinguish the kernel's advertised/timer cadence, CADisplayLink delivery,
 * and physical panel refresh (the last is NOT measured by this program).
 * Usage: fb_rate_test [sample seconds, default 5] [startup delay, default 40].
 */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <objc/runtime.h>
#include <string.h>
extern const void *_dyld_get_shared_cache_range(size_t *length);

static void display_configuration(void)
{
    Class cls = NSClassFromString(@"CADisplay");
    SEL selector = NSSelectorFromString(@"mainDisplay");
    if (!cls || ![cls respondsToSelector:selector]) return;
    id display = [cls performSelector:selector];
    printf("fb_rate_test: CADisplay %s\n", [[display description] UTF8String]);
    unsigned method_count = 0;
    Method *methods = class_copyMethodList([display class], &method_count);
    for (unsigned i=0; i<method_count; i++) {
        const char *name=sel_getName(method_getName(methods[i]));
        if (strcasestr(name,"frame") || strcasestr(name,"mode") || strcasestr(name,"refresh") || strcasestr(name,"timing"))
            printf("fb_rate_test: CADisplay.method %s ABI %s\n",name,method_getTypeEncoding(methods[i]));
    }
    free(methods);
    const char *mode_names[]={"availableModes","supportedModes","currentMode","preferredMode","displayMode","configuration"};
    for(unsigned i=0;i<sizeof(mode_names)/sizeof(mode_names[0]);i++) {
        SEL getter=sel_registerName(mode_names[i]);
        NSMethodSignature *signature=[display methodSignatureForSelector:getter];
        if(!signature || signature.numberOfArguments!=2 || signature.methodReturnType[0]!='@') continue;
        NSInvocation *call=[NSInvocation invocationWithMethodSignature:signature];
        call.target=display;call.selector=getter;
        @try {
            [call invoke];
            __unsafe_unretained id value=nil; [call getReturnValue:&value];
            printf("fb_rate_test: CADisplay.%s %s\n",mode_names[i],[[value description] UTF8String]);
        } @catch(NSException *e) {
            printf("fb_rate_test: CADisplay.%s exception %s\n",mode_names[i],e.name.UTF8String);
        }
    }
    const char *names[] = {"refreshRate", "nominalRefreshRate", "minimumRefreshRate",
        "maximumRefreshRate", "minimumFrameDuration", "maximumFrameDuration",
        "preferredFrameRate", "currentFrameRate", "maximumFramesPerSecond"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        SEL getter = sel_registerName(names[i]);
        NSMethodSignature *signature = [display methodSignatureForSelector:getter];
        if (!signature || signature.numberOfArguments != 2) continue;
        const char *type = signature.methodReturnType;
        size_t cache_size = 0;
        uintptr_t slide = (uintptr_t)_dyld_get_shared_cache_range(&cache_size) - 0x180000000ULL;
        IMP implementation = class_getMethodImplementation([display class], getter);
        printf("fb_rate_test: CADisplay.%s implementation=%#llx unslid\n", names[i], (unsigned long long)((uintptr_t)implementation - slide));
        if (strcmp(type, "d") && strcmp(type, "f") && strcmp(type, "q") && strcmp(type, "Q") && strcmp(type, "i") && strcmp(type, "I")) continue;
        NSInvocation *invocation = [NSInvocation invocationWithMethodSignature:signature];
        invocation.target = display; invocation.selector = getter;
        [invocation invoke];
        double value = 0;
        if (!strcmp(type, "d")) [invocation getReturnValue:&value];
        else if (!strcmp(type, "f")) { float n; [invocation getReturnValue:&n]; value = n; }
        else if (!strcmp(type, "q")) { int64_t n; [invocation getReturnValue:&n]; value = n; }
        else if (!strcmp(type, "Q")) { uint64_t n; [invocation getReturnValue:&n]; value = n; }
        else { int32_t n; [invocation getReturnValue:&n]; value = n; }
        printf("fb_rate_test: CADisplay.%s=%g (ABI %s)\n", names[i], value, type);
    }
}

static uint64_t first_tick, last_tick, first_count, last_count, callbacks;
static void vsync_callback(void *context, IOReturn status, void **arguments, uint32_t count)
{
    (void)context;
    if (status != kIOReturnSuccess || count != 6) return;
    uint64_t tick = (uint32_t)(uintptr_t)arguments[0] | ((uint64_t)(uint32_t)(uintptr_t)arguments[1] << 32);
    uint64_t number = (uint32_t)(uintptr_t)arguments[4] | ((uint64_t)(uint32_t)(uintptr_t)arguments[5] << 32);
    if (!callbacks) { first_tick = tick; first_count = number; }
    last_tick = tick; last_count = number; callbacks++;
}

@interface FrameProbe : NSObject
@property NSUInteger frames;
@property CFTimeInterval first;
@property CFTimeInterval last;
- (void)tick:(CADisplayLink *)link;
@end
@implementation FrameProbe
- (void)tick:(CADisplayLink *)link
{
    if (!_frames) _first = link.timestamp;
    _last = link.timestamp;
    _frames++;
}
@end

int main(int argc, char **argv)
{
    @autoreleasepool {
        double seconds = argc > 1 ? atof(argv[1]) : 5;
        int delay = argc > 2 ? atoi(argv[2]) : 40;
        if (seconds < 1 || seconds > 10 || delay < 0 || delay > 120) return 1;
        setvbuf(stdout, NULL, _IONBF, 0);
        sleep((unsigned)delay);
        io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("IOMobileFramebuffer"));
        if (!service) { puts("fb_rate_test: no framebuffer service"); return 1; }
        CFTypeRef timing = IORegistryEntryCreateCFProperty(service, CFSTR("TimingElements"), NULL, 0);
        if (timing) {
            CFDataRef xml = CFPropertyListCreateXMLData(NULL, timing);
            if (xml) {
                printf("fb_rate_test: advertised timings %.*s\n", (int)CFDataGetLength(xml), CFDataGetBytePtr(xml));
                CFRelease(xml);
            }
            CFRelease(timing);
        }
        io_connect_t connection = 0;
        IOReturn status = IOServiceOpen(service, mach_task_self(), 0, &connection);
        IOObjectRelease(service);
        if (status != kIOReturnSuccess) { printf("fb_rate_test: open %#x\n", status); return 1; }
        IONotificationPortRef port = IONotificationPortCreate(0);
        if (!port) { IOServiceClose(connection); return 1; }
        CFRunLoopSourceRef source = IONotificationPortGetRunLoopSource(port);
        if (!source) { IONotificationPortDestroy(port); IOServiceClose(connection); return 1; }
        CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
        status = IOConnectSetNotificationPort(connection, 5, IONotificationPortGetMachPort(port), 0);
        uint64_t input[4] = {(uintptr_t)&vsync_callback, 0, 5, 0};
        if (status == kIOReturnSuccess) status = IOConnectCallScalarMethod(connection, 72, input, 4, NULL, NULL);
        if (status == kIOReturnSuccess) CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
        input[0] = 0;
        IOConnectCallScalarMethod(connection, 72, input, 4, NULL, NULL);
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        double elapsed = (double)(last_tick - first_tick) * tb.numer / tb.denom / 1e9;
        printf("fb_rate_test: kernel timer registration=%#x callbacks=%llu ticks=%llu sequence=%llu rate=%.3f Hz\n",
            status, (unsigned long long)callbacks, (unsigned long long)(last_tick - first_tick),
            (unsigned long long)(last_count - first_count), elapsed > 0 ? (last_count - first_count) / elapsed : 0);
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
        IONotificationPortDestroy(port);
        IOServiceClose(connection);
        printf("fb_rate_test: UIScreen.maximumFramesPerSecond=%ld\n", (long)UIScreen.mainScreen.maximumFramesPerSecond);
        display_configuration();
        FrameProbe *probe = [FrameProbe new];
        CADisplayLink *link = [CADisplayLink displayLinkWithTarget:probe selector:@selector(tick:)];
        link.preferredFrameRateRange = CAFrameRateRangeMake(120, 120, 120);
        [link addToRunLoop:NSRunLoop.currentRunLoop forMode:NSDefaultRunLoopMode];
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
        [link invalidate];
        printf("fb_rate_test: CADisplayLink callbacks=%lu rate=%.3f Hz%s\n", (unsigned long)probe.frames,
            probe.frames > 1 && probe.last > probe.first ? (probe.frames - 1) / (probe.last - probe.first) : 0,
            probe.frames ? "" : " (standalone client inactive; not measured)");
        puts("fb_rate_test: software cadence only; physical panel rate and GPU-rendered FPS are separate measurements");
        [probe release];
        return status == kIOReturnSuccess && callbacks > 1 ? 0 : 1;
    }
}
