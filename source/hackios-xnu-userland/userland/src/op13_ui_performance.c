/* Optional SpringBoard-only profile for the CPU-rendered OnePlus13 UI.
 * Keep tint, color and layout; omit the expensive backdrop blur filters.
 * Loaded only by payloads built with --reduce-effects.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <string.h>

static void (*original_radius)(id, SEL, double);
static void (*original_filters)(id, SEL, id);
static void (*original_backdrop_filters)(id, SEL, id);
static unsigned radii, removed;
static void (*original_dock_layout)(id, SEL);
static unsigned dock_changes;

static void dock_layout(id self, SEL sel)
{
    original_dock_layout(self, sel);
    const char *names[] = { "_backgroundView", "_backgroundEffectView", "_dockBackgroundView" };
    unsigned hidden = 0;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        Ivar ivar = class_getInstanceVariable(object_getClass(self), names[i]);
        if (!ivar || ivar_getTypeEncoding(ivar)[0] != '@') continue;
        id background = object_getIvar(self, ivar);
        if (background && class_respondsToSelector(object_getClass(background), sel_registerName("setHidden:"))) {
            ((void (*)(id, SEL, BOOL))objc_msgSend)(background, sel_registerName("setHidden:"), YES);
            hidden++;
        }
    }
    if (hidden && dock_changes++ == 0)
        fprintf(stderr, "op13-performance: dock background views hidden=%u; icons retained\n", hidden);
}

static void radius(id self, SEL sel, double value)
{
    if (value > 0 && radii++ == 0)
        fprintf(stderr, "op13-performance: backdrop blur radius disabled\n");
    original_radius(self, sel, 0.0);
}

static id without_blur(id filters)
{
    if (!filters) return filters;
    SEL count = sel_registerName("count"), at = sel_registerName("objectAtIndex:");
    SEL type = sel_registerName("type"), utf8 = sel_registerName("UTF8String");
    unsigned long n = ((unsigned long (*)(id, SEL))objc_msgSend)(filters, count);
    id keep = ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("NSMutableArray"), sel_registerName("array"));
    if (!keep) return filters;
    for (unsigned long i = 0; i < n; ++i) {
        id f = ((id (*)(id, SEL, unsigned long))objc_msgSend)(filters, at, i);
        const char *name = NULL;
        if (class_respondsToSelector(object_getClass(f), type)) {
            id t = ((id (*)(id, SEL))objc_msgSend)(f, type);
            if (t && class_respondsToSelector(object_getClass(t), utf8))
                name = ((const char *(*)(id, SEL))objc_msgSend)(t, utf8);
        }
        if (name && (!strcmp(name, "gaussianBlur") || !strcmp(name, "variableBlur"))) {
            if (removed++ == 0)
                fprintf(stderr, "op13-performance: removed %s filter; color filters retained\n", name);
        } else {
            ((void (*)(id, SEL, id))objc_msgSend)(keep, sel_registerName("addObject:"), f);
        }
    }
    return keep;
}

static void filters(id self, SEL sel, id value)
{
    original_filters(self, sel, without_blur(value));
}

static void backdrop_filters(id self, SEL sel, id value)
{
    original_backdrop_filters(self, sel, without_blur(value));
}

/* Isolate inherited methods on the requested class before replacing them. */
static IMP hook(const char *class_name, const char *selector, IMP replacement, char argument)
{
    Class c = objc_getClass(class_name);
    SEL s = sel_registerName(selector);
    Method m = c ? class_getInstanceMethod(c, s) : NULL;
    if (!m || method_getNumberOfArguments(m) != 3) return NULL;
    char result[8], arg[8];
    method_getReturnType(m, result, sizeof(result));
    method_getArgumentType(m, 2, arg, sizeof(arg));
    if (result[0] != 'v' || arg[0] != argument) return NULL;
    IMP original = method_getImplementation(m);
    if (!class_addMethod(c, s, replacement, method_getTypeEncoding(m)))
        method_setImplementation(m, replacement);
    return original;
}

__attribute__((constructor)) static void start(void)
{
    original_radius = (void *)hook("_UIBackdropViewSettings", "setBlurRadius:", (IMP)radius, 'd');
    original_filters = (void *)hook("CALayer", "setFilters:", (IMP)filters, '@');
    original_backdrop_filters = (void *)hook("CABackdropLayer", "setFilters:", (IMP)backdrop_filters, '@');
    Class dock = objc_getClass("SBDockView");
    SEL layout = sel_registerName("layoutSubviews");
    Method m = dock ? class_getInstanceMethod(dock, layout) : NULL;
    if (m && method_getNumberOfArguments(m) == 2) {
        char result[8]; method_getReturnType(m, result, sizeof(result));
        if (result[0] == 'v') {
            original_dock_layout = (void *)method_getImplementation(m);
            if (!class_addMethod(dock, layout, (IMP)dock_layout, method_getTypeEncoding(m)))
                method_setImplementation(m, (IMP)dock_layout);
        }
    }
    fprintf(stderr, "op13-performance: CPU rendering profile radius=%s filters=%s backdrop=%s\n",
        original_radius ? "on" : "unavailable", original_filters ? "on" : "unavailable",
        original_backdrop_filters ? "on" : "unavailable");
    fprintf(stderr, "op13-performance: direct dock layout hook=%s\n", original_dock_layout ? "on" : "unavailable");
}
