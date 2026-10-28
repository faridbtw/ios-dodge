/* Poll the system UI brightness; send only changed, validated levels.
 * BKSDisplayBrightnessGetCurrent: float(void), verified in iOS16.5.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
 setbuf(stdout,0);setbuf(stderr,0);
 void *bks=dlopen("/System/Library/PrivateFrameworks/BackBoardServices.framework/BackBoardServices",RTLD_NOW);
 float (*current)(void)=bks?(float (*)(void))dlsym(bks,"BKSDisplayBrightnessGetCurrent"):0;
 if(!current){fprintf(stderr,"op13-screen-agent: BKS getter unavailable\n");return 1;}
 fprintf(stderr,"op13-screen-agent: BKS polling started\n");
 int fd=-1;uint32_t last=~0u;
 for(;;){
  if(fd<0)fd=open("/dev/op13-screen",O_WRONLY);
  float value=current();
  if(fd>=0&&isfinite(value)&&value>=0.0f&&value<=1.0f){
   uint32_t level=(uint32_t)(value*1000.0f+0.5f);
   if(level!=last){
    if(write(fd,&level,sizeof(level))!=sizeof(level)){
     perror("op13-screen-agent: write");close(fd);return 2;
    }
    fprintf(stderr,"op13-screen-agent: brightness=%u/1000\n",level);last=level;
   }
  }
  CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.001,false);
  usleep(100000);
 }
}
