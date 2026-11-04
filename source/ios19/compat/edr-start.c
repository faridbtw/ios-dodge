/* iOS19's stock EDR IPC service: initialize it in its owning backboardd process.
 * The exact target's EDRServerStartService was inspected: no arguments, no hardware descriptor.
 * This supplies the native server; it does not replace client answers or advertise HDR hardware.
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <mach/mach_time.h>
__attribute__((constructor)) static void start_native_edr(void)
{
    const char *name=getprogname();
    char args[1024]={0};size_t n=sizeof(args)-1;
    if(!name || strcmp(name,"backboardd") || sysctlbyname("kern.bootargs",args,&n,NULL,0) || !strstr(args,"edr_server=1"))return;
    void *lib=dlopen("/System/Library/PrivateFrameworks/libEDR.framework/libEDR",RTLD_NOW|RTLD_LOCAL);
    void (*start)(void)=lib?(void (*)(void))dlsym(lib,"EDRServerStartService"):NULL;
    int con=open("/dev/console",O_WRONLY|O_NOCTTY);
    char line[192];
    if(!start){snprintf(line,sizeof(line),"edr-start: native service unavailable\n");}
    else{
        uint64_t before=mach_absolute_time();start();mach_timebase_info_data_t tb;mach_timebase_info(&tb);
        uint64_t us=(mach_absolute_time()-before)*tb.numer/tb.denom/1000;
        snprintf(line,sizeof(line),"edr-start: native EDRServerStartService returned in %llu us\n",(unsigned long long)us);
    }
    write(2,line,strlen(line));if(con>=0){write(con,line,strlen(line));close(con);}
}
