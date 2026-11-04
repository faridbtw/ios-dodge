#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <stdatomic.h>

/* iOS 27's FSNode requires NSURLCanonicalPathKey. The current filesystem
 * backend sometimes reports success with a nil value. Supply only that
 * missing result, from POSIX realpath, preserving native errors and all
 * other resource properties. MRC preserves the resource getter convention. */
static BOOL (*originalGet)(id,SEL,id *,NSString *,NSError **);
static atomic_uint reports;
static BOOL getVerifiedPath(NSURL *self,SEL selector,id *value,NSString *key,NSError **error)
{
    BOOL ok=originalGet(self,selector,value,key,error);
    if (ok && value && *value==nil && [key isEqualToString:NSURLCanonicalPathKey] && [self isFileURL]) {
        char resolved[PATH_MAX];
        if (realpath([self fileSystemRepresentation],resolved)) {
            *value=[NSString stringWithUTF8String:resolved];
            if (atomic_fetch_add(&reports,1)<16)
                fprintf(stderr,"ls-path: returned verified canonical path %s\n",resolved);
        } else if (atomic_fetch_add(&reports,1)<16)
            fprintf(stderr,"ls-path: realpath failed errno=%d url=%s\n",errno,[[self path] UTF8String]);
    }
    return ok;
}
__attribute__((constructor)) static void install(void)
{
    Class cls=object_getClass([NSURL fileURLWithPath:@"/"]);
    Method method=class_getInstanceMethod(cls,@selector(getResourceValue:forKey:error:));
    if (method) {
        originalGet=(void *)method_getImplementation(method);
        method_setImplementation(method,(IMP)getVerifiedPath);
        fprintf(stderr,"ls-path: canonical getter fallback installed on %s, encoding=%s\n",class_getName(cls),method_getTypeEncoding(method));
    }
}
