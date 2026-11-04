#include "mini.h"
static void ensure(const char *p) {
 struct stat s;
 if(stat(p,&s)==0)return;
 int result=sysctlbyname("kern.apfs_synthetic",0,0,p,strlen(p));
 printf("solarium-cryptex: directory %s result=%d errno=%d\n",p,result,result?errno:0);
}
static int overlay(const char *source,const char *at) {
 int result=mount("bindfs",at,0x20 /* MNT_UNION */,(void *)source);
 printf("solarium-cryptex: original-files overlay %s on %s result=%d errno=%d\n",source,at,result,result?errno:0);
 return result;
}
int main(int argc,char **argv,char **envp) {
 (void)argc;(void)argv;
 ensure("/System/Cryptexes/OS/usr");ensure("/System/Cryptexes/OS/usr/lib");ensure("/System/Cryptexes/OS/usr/local");ensure("/usr/local");
 overlay("/redwood/cryptex/usr/lib","/System/Cryptexes/OS/usr/lib");
 overlay("/redwood/cryptex/usr/lib","/usr/lib");
 overlay("/redwood/cryptex/usr/local","/System/Cryptexes/OS/usr/local");
 overlay("/redwood/cryptex/usr/local","/usr/local");
 ensure("/System/Cryptexes/OS/System");
 ensure("/System/Cryptexes/OS/System/Library");
 ensure("/System/Cryptexes/OS/System/Library/PrivateFrameworks");
 overlay("/redwood/native-support/System/Library/PrivateFrameworks","/System/Library/PrivateFrameworks");
 overlay("/redwood/native-support/System/Library/PrivateFrameworks","/System/Cryptexes/OS/System/Library/PrivateFrameworks");
 overlay("/redwood/native-support/System/Library/LaunchDaemons","/System/Library/LaunchDaemons");
 overlay("/redwood/native-support/System/Library/FeatureFlags/Domain","/System/Library/FeatureFlags/Domain");
 overlay("/redwood/ui-artwork","/System/Library/PrivateFrameworks/UIKitCore.framework/Artwork.bundle");
 struct stat s;int ok=stat("/usr/lib/libobjc-trampolines.dylib",&s)==0 && s.st_size==68176;
 printf("solarium-cryptex: original objc trampoline available=%d\n",ok);
 ensure("/System/Library/Extensions");
 ensure("/System/Library/Extensions/RedwoodMetal.bundle");
 overlay("/redwood/lib/RedwoodMetal.bundle","/System/Library/Extensions/RedwoodMetal.bundle");
 pid_t child=fork();
 if(child==0){char *ma[]={"/redwood/bin/metal_props","on","RedwoodMetal","NativeAdrenoMetalDevice",0};execve(ma[0],ma,envp);return 1;}
 int status=0;pid_t done=child>0?wait4(child,&status,0,0):-1;
 printf("solarium-metal: discovery registration child=%d waited=%d status=%d\n",child,done,status);
 char *args[]={"/sbin/launchd.ios",0};
 execve(args[0],args,envp);
 printf("solarium-cryptex: native launchd exec failed errno=%d\n",errno);
 return 1;
}
