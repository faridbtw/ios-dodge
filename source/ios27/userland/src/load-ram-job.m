#import <Foundation/Foundation.h>
#include <launch.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

static launch_data_t encode(id value)
{
    if ([value isKindOfClass:[NSDictionary class]]) {
        launch_data_t out=launch_data_alloc(LAUNCH_DATA_DICTIONARY);
        for (NSString *key in value) {
            launch_data_t item=encode([value objectForKey:key]);
            if (!item || !launch_data_dict_insert(out,item,[key UTF8String])) { launch_data_free(out); return NULL; }
        }
        return out;
    }
    if ([value isKindOfClass:[NSArray class]]) {
        launch_data_t out=launch_data_alloc(LAUNCH_DATA_ARRAY);
        for (NSUInteger i=0;i<[value count];i++) {
            launch_data_t item=encode([value objectAtIndex:i]);
            if (!item || !launch_data_array_set_index(out,item,i)) { launch_data_free(out); return NULL; }
        }
        return out;
    }
    if ([value isKindOfClass:[NSString class]]) return launch_data_new_string([value UTF8String]);
    if ([value isKindOfClass:[NSNumber class]]) {
        if (CFGetTypeID((CFTypeRef)value)==CFBooleanGetTypeID()) return launch_data_new_bool([value boolValue]);
        return launch_data_new_integer([value longLongValue]);
    }
    return NULL;
}
int main(int argc,char **argv)
{
    @autoreleasepool {
        if (argc!=2) return 2;
        NSDictionary *job=[NSDictionary dictionaryWithContentsOfFile:[NSString stringWithUTF8String:argv[1]]];
        launch_data_t value=job ? encode(job) : NULL;
        if (!value) {fprintf(stderr,"ram-job: invalid job %s\n",argv[1]);return 2;}
        launch_data_t request=launch_data_alloc(LAUNCH_DATA_DICTIONARY);
        launch_data_dict_insert(request,value,LAUNCH_KEY_SUBMITJOB);
        launch_data_t reply=launch_msg(request);
        int error=reply ? (launch_data_get_type(reply)==LAUNCH_DATA_ERRNO ? launch_data_get_errno(reply) : EPROTO) : errno;
        fprintf(stderr,"ram-job: submit %s result=%d (%s)\n",[[job objectForKey:@"Label"] UTF8String],error,strerror(error));
        if(reply)launch_data_free(reply);
        launch_data_free(request);
        return error!=0;
    }
}
