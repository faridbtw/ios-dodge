#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <IOKit/IOKitLib.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
static BOOL (*originalSetup)(id,SEL);
static BOOL inspectSetup(id self,SEL selector)
{
    BOOL result=originalSetup(self,selector);
    Ivar iv=class_getInstanceVariable(object_getClass(self),"_displayService");
    io_service_t service=0;
    if(iv)memcpy(&service,(char *)self+ivar_getOffset(iv),sizeof(service));
    io_name_t name={0},className={0};
    if(service){IORegistryEntryGetName(service,name);IOObjectGetClass(service,className);}
    fprintf(stderr,"cb-display: setup=%d service=%u name=%s class=%s\n",result,service,name,className);
    const char *fields[]={"_display","_context","_brtCtl","_displayModule","_displayControlModule","_modules","_displayID","_displayUUID"};
    for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);i++){
        Ivar field=class_getInstanceVariable(object_getClass(self),fields[i]);
        if(!field || ivar_getTypeEncoding(field)[0]!='@')continue;
        id value=object_getIvar(self,field);
        fprintf(stderr,"cb-display: %s class=%s value=%s\n",fields[i],value?class_getName(object_getClass(value)):"nil",[[value description] UTF8String]);
    }
    return result;
}
__attribute__((constructor)) static void install(void)
{
    dlopen("/System/Library/PrivateFrameworks/CoreBrightness.framework/CoreBrightness",RTLD_LAZY);
    Method method=class_getInstanceMethod(NSClassFromString(@"CBDisplayContaineriOS"),sel_registerName("findBacklight"));
    if(method){originalSetup=(void *)method_getImplementation(method);method_setImplementation(method,(IMP)inspectSetup);fprintf(stderr,"cb-display: findBacklight diagnostic installed\n");}
}
