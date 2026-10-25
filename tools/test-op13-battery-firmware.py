#!/usr/bin/env python3
"""Run the production ADSP metadata validator against phone firmware and faults."""
from pathlib import Path
import subprocess
import tempfile

r = Path(__file__).resolve().parents[1]
s = (r / 'source/xnu-build-snapshot/xnu-8796.121.2/bsd/dev/op13_battery_adsp.c').read_text()
structs = s[s.index('#define MAX_SEGMENTS'):s.index('static volatile bool stopping, boot_runs, link_failed;')]
fn = s[s.index('static bool\nfirmware_check('):s.index('static bool\npas_load(')]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
#define PAS_ADSP 1
#define PAS_DTB 36
#define ADSP_MEMORY 0x9ee00000u
#define ADSP_MEMORY_SIZE 0x05080000u
#define DTB_MEMORY 0x9ed80000u
#define DTB_MEMORY_SIZE 0x80000u
static const char *directory;
static unsigned scenario;
static bool from_root;
static int log_message(const char *format,...) {(void)format;return 0;}
#define printf log_message
static uint64_t mach_absolute_time(void) {return 0;}
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *out) {*out=t;}
static const uint8_t *image_file(const char *name,uint64_t *size) {
 char path[1024];snprintf(path,sizeof(path),"%s/%s",directory,name);
 bool metadata=strstr(name,".mdt")!=NULL;
 if((scenario==1 && metadata)||(scenario==5 && !metadata))return NULL;
 FILE *f=fopen(path,"rb");assert(f);
 assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0);rewind(f);
 uint8_t *b=malloc((size_t)n);assert(b);assert(fread(b,1,(size_t)n,f)==(size_t)n);fclose(f);
 *size=(uint64_t)n;from_root=true;
 if(metadata) {
  if(scenario==2)b[0]=0;
  if(scenario==3){uint32_t bad=0xfffffff0;memcpy(b+28,&bad,4);}
  if(scenario==4)b[44]=255;
  if(scenario==7)*size=51;
 } else if(scenario==6)(*size)--;
 return b;
}
static void file_free(const uint8_t *data,uint64_t size,bool owned) {(void)size;if(owned)free((void *)data);}
''' + structs + fn + r'''
int main(int argc,char **argv) {
 assert(argc==2);directory=argv[1];
 for(unsigned image=0;image<2;image++) {
  for(scenario=0;scenario<9;scenario++) {
   struct pas_image im=images[image];
   if(scenario==8)im.capacity=4096;
   bool result=firmware_check(&im);
   assert(result==(scenario==0));
   if(result)assert(im.span<=im.capacity && im.base==images[image].base);
   if(im.owned)free((void *)im.mdt);
  }
 }
 puts("18 production firmware validation cases passed on captured ADSP/DTB files");
}
'''
with tempfile.TemporaryDirectory() as d:
    c = Path(d) / 'firmware.c'; c.write_text(code)
    exe = Path(d) / 'firmware'
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe), str(r / 'diagnostics/20261025-battery/firmware')], check=True)
