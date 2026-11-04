#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <stdatomic.h>
static BOOL (*original)(id,SEL,id *,id,unsigned long,NSError **);
static atomic_uint failures;
static BOOL inspectResource(id self,SEL selector,id *value,id key,unsigned long options,NSError **error)
{
    BOOL ok=original(self,selector,value,key,options,error);
    if (!ok && atomic_fetch_add(&failures,1)<24)
        fprintf(stderr,"ls-resource: unavailable key=%s options=%lu error=%s\n",[[key description] UTF8String],options,error ? [[*error description] UTF8String] : "not requested");
    return ok;
}
__attribute__((constructor)) static void install(void)
{
    Class cls=NSClassFromString(@"FSNode");
    Method method=class_getInstanceMethod(cls,sel_registerName("getResourceValue:forKey:options:error:"));
    if (method) {
        original=(void *)method_getImplementation(method);
        method_setImplementation(method,(IMP)inspectResource);
        fprintf(stderr,"ls-resource: diagnostic installed; encoding=%s\n",method_getTypeEncoding(method));
    } else fprintf(stderr,"ls-resource: FSNode method absent\n");
}
