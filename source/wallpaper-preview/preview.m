/* SPDX-License-Identifier: BSD-2-Clause
 * iOS20F66 PosterBoard preview capture for the GLES-only OnePlus13 port.
 * Replace only the verified Metal-dependent _imageForView: implementation.
 */
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#include <mach-o/dyld.h>
#include <objc/runtime.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>
#include <math.h>
#include <stdlib.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);
static BOOL installed;
static unsigned captures;

static void tell(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    int fd = open("/dev/console", O_WRONLY | O_NOCTTY);
    if (fd >= 0) {
        if (n > 0) write(fd, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line)-1);
        close(fd);
    }
}

static UIImage *capture(id self, SEL command, UIView *view)
{
    (void)self; (void)command;
    if (view == nil) return nil;
    CGSize size = view.bounds.size;
    CGFloat scale = view.layer.contentsScale;
    if (!isfinite(scale) || scale < 1 || scale > 3) scale = 1;
    if (!isfinite(size.width) || !isfinite(size.height) ||
        size.width <= 0 || size.height <= 0 ||
        size.width * scale > 8192 || size.height * scale > 8192 ||
        size.width * size.height * scale * scale > 32 * 1024 * 1024) {
        tell("op13-wallpaper: declined invalid preview dimensions\n");
        return nil;
    }
    UIImage *result = nil;
    UIGraphicsBeginImageContextWithOptions(size, NO, scale);
    @try {
        CGContextRef context = UIGraphicsGetCurrentContext();
        if (context != NULL) {
            [view.layer renderInContext:context];
            result = UIGraphicsGetImageFromCurrentImageContext();
        }
    } @catch (NSException *error) {
        tell("op13-wallpaper: CoreGraphics preview failed: %s\n", error.name.UTF8String);
    } @finally {
        UIGraphicsEndImageContext();
    }
    unsigned number = __atomic_fetch_add(&captures, 1, __ATOMIC_RELAXED);
    if (number < 24) {
        tell("op13-wallpaper: CoreGraphics preview %u %.0fx%.0f scale=%.0f image=%s\n",
             number + 1, size.width, size.height, scale, result ? "ready" : "missing");
    }
    return result;
}

static void image_added(const struct mach_header *header, intptr_t image_slide)
{
    (void)header; (void)image_slide;
    if (installed) return;
    size_t length = 0;
    uintptr_t cache = (uintptr_t)_dyld_get_shared_cache_range(&length);
    if (!cache || !length) return;
    uintptr_t expected = 0x1c0a0f8fcULL + cache - 0x180000000ULL;
    SEL selector = sel_registerName("_imageForView:");
    int capacity = objc_getClassList(NULL, 0);
    if (capacity <= 0 || capacity > 100000) return;
    Class *classes = calloc((size_t)capacity, sizeof(Class));
    if (classes == NULL) return;
    int count = objc_getClassList(classes, capacity);
    if (count > capacity) count = capacity;
    for (int i = 0; i < count && !installed; i++) {
        for (unsigned meta = 0; meta < 2 && !installed; meta++) {
            Class owner = meta ? object_getClass(classes[i]) : classes[i];
            Method method = class_getInstanceMethod(owner, selector);
            char result_type[16] = {0}, view_type[16] = {0};
            if (method) {
                method_getReturnType(method, result_type, sizeof(result_type));
                if (method_getNumberOfArguments(method) == 3)
                    method_getArgumentType(method, 2, view_type, sizeof(view_type));
            }
            if (method && result_type[0] == '@' && view_type[0] == '@' &&
                (uintptr_t)method_getImplementation(method) == expected) {
                method_setImplementation(method, (IMP)capture);
                installed = YES;
                tell("op13-wallpaper: %s[%s _imageForView:] verified; CoreGraphics preview installed\n",
                     meta ? "+" : "-", class_getName(classes[i]));
            }
        }
    }
    free(classes);
}

__attribute__((constructor)) static void start(void)
{
    tell("op13-wallpaper: scoped preview helper loaded\n");
    _dyld_register_func_for_add_image(image_added);
}
