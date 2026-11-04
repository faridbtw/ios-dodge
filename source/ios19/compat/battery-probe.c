#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <sys/sysctl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
static int value(const char *key) {int v=-999;size_t n=sizeof v;if(sysctlbyname(key,&v,&n,NULL,0))return -999;return v;}
static void show(int fd,const char *scope,CFDictionaryRef d,const char *key) {
 CFStringRef k=CFStringCreateWithCString(NULL,key,kCFStringEncodingUTF8);CFTypeRef v=d?CFDictionaryGetValue(d,k):NULL;
 CFRelease(k);if(!v)return;CFStringRef desc=CFCopyDescription(v);char b[256];if(desc&&CFStringGetCString(desc,b,sizeof b,kCFStringEncodingUTF8))dprintf(fd,"battery-probe: %s %s=%s\n",scope,key,b);if(desc)CFRelease(desc);
}
int main(void) {
 int fd=open("/dev/console",O_WRONLY);if(fd<0)return 1;
 void *lib=dlopen("/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit",RTLD_LAZY);
 CFTypeRef (*info)(void)=lib?dlsym(lib,"IOPSCopyPowerSourcesInfo"):NULL;
 CFArrayRef (*list)(CFTypeRef)=lib?dlsym(lib,"IOPSCopyPowerSourcesList"):NULL;
 CFDictionaryRef (*description)(CFTypeRef,CFTypeRef)=lib?dlsym(lib,"IOPSGetPowerSourceDescription"):NULL;
 sleep(15);
 for(unsigned n=0;n<30;n++) {
  dprintf(fd,"battery-probe: n=%u soc=%d voltage=%d current=%d temperature=%d external=%d charging=%d fromDSP=%d\n",n,value("kern.redwood.battery_soc"),value("kern.redwood.battery_voltage_mv"),value("kern.redwood.battery_current_ma"),value("kern.redwood.battery_temperature_c"),value("kern.redwood.battery_charger_present"),value("kern.redwood.battery_charging"),value("kern.redwood.battery_from_dsp"));
  io_iterator_t it=0;
  if(IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching("IOPMPowerSource"),&it)==KERN_SUCCESS){
   io_service_t svc;
   while((svc=IOIteratorNext(it))){
    CFMutableDictionaryRef props=NULL;
    if(IORegistryEntryCreateCFProperties(svc,&props,kCFAllocatorDefault,0)==KERN_SUCCESS){
     for(unsigned i=0;i<4;i++)show(fd,"registry",props,(const char *[]){"CurrentCapacity","MaxCapacity","ExternalConnected","IsCharging"}[i]);CFRelease(props);
    }
    IOObjectRelease(svc);
   }
   IOObjectRelease(it);
  }
  if(info&&list&&description){CFTypeRef blob=info();if(blob){CFArrayRef sources=list(blob);if(sources){dprintf(fd,"battery-probe: native power-source count=%ld\n",CFArrayGetCount(sources));for(CFIndex i=0;i<CFArrayGetCount(sources);i++){CFDictionaryRef d=description(blob,CFArrayGetValueAtIndex(sources,i));for(unsigned j=0;j<4;j++)show(fd,"IOPS",d,(const char *[]){"Current Capacity","Max Capacity","Is Charging","Power Source State"}[j]);}CFRelease(sources);}CFRelease(blob);}}
  sleep(5);
 }
 close(fd);return 0;
}
