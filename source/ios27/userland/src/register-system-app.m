#import <Foundation/Foundation.h>
#include <stdio.h>
#include <unistd.h>

/* Private APIs declared so ARC applies the init ownership convention correctly. */
@interface NSObject (RAMApplicationRegistration)
+ (id)defaultWorkspace;
- (BOOL)registerApplication:(NSURL *)url;
- (id)initWithBundleIdentifier:(NSString *)identifier allowPlaceholder:(BOOL)placeholder error:(NSError **)error;
@end

int main(void)
{
    sleep(12);
    int g=setgid(501), u=setuid(501);
    printf("appreg: starting in mobile session gid=%d uid=%d\n",g,u);
    fflush(stdout);
    if (g || u) return 3;
    alarm(30);
    @autoreleasepool {
        id workspace = [NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace];
        NSURL *url = [NSURL fileURLWithPath:@"/System/Library/CoreServices/SpringBoard.app" isDirectory:YES];
        for (unsigned attempt=0; attempt<3; attempt++) {
            BOOL ok = [workspace registerApplication:url];
            printf("appreg: SpringBoard registration attempt %u: %s\n",attempt+1,ok ? "success" : "failed");
            fflush(stdout);
            NSError *error=nil;
            id record = [[NSClassFromString(@"LSApplicationRecord") alloc] initWithBundleIdentifier:@"com.apple.springboard" allowPlaceholder:NO error:&error];
            printf("appreg: record %s; error %s; details %s\n",[[record description] UTF8String],[[error description] UTF8String],[[[error userInfo] description] UTF8String]);
            fflush(stdout);
            if (record) return 0;
            sleep(3);
        }
    }
    return 1;
}
