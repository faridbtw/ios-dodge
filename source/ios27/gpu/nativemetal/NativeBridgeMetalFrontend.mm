// SPDX-License-Identifier: MIT
// Native Metal API frontend over the bridge (bridge/NativeMetalObjects.h): the metal2vulkan runtime on Turnip, on the Adreno 740 through
// NativeAdreno. Same shape as the offline frontend: the device is a dynamically allocated subclass of the target's own _MTLDevice that
// only adds this file's implementations; every other object is an own class answering the Metal protocol methods it implements.
// Every result comes from the runtime and the GPU: a refused call returns nil/NO with the runtime's reason, an encoder call the runtime
// refuses fails its command buffer (status Error, handlers run) instead of being dropped. A selector this frontend does not implement is
// reported through the missing-selector hook before the usual exception, so a hardware run names the gap.
// Shared events work within this process (no cross-process handle yet). Not yet implemented here: heaps, argument buffers, indirect commands, drawables/presentation.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#import <objc/runtime.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <map>
#include <pthread.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/mman.h>
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <execinfo.h>
#include <sys/sysctl.h>
#include <objc/message.h>
static int pandoraAstcProbeLevel() {   // data-a-live: boot-arg pandora_astc_probe=N (tools/metal/adapt_ios16.py)
 static const int level=[]{char args[1024]={};size_t size=sizeof args-1;
  if(sysctlbyname("kern.bootargs",args,&size,nullptr,0)!=0)return 0;
  const char *at=strstr(args,"pandora_astc_probe=");return at?atoi(at+19):0;}();
 return level;
}
static bool pandoraAstcProbe() {return pandoraAstcProbeLevel()>=1;}
static char gProbePassTargetKey,gProbeReadbackKey,gProbeSamplerKey,gProbeBindingsKey,gProbePipeKey;
// data-a-live: an encoder's bindings as the probe's draw log prints them (tools/metal/adapt_ios16.py).
static NSMutableDictionary *probeBindings(id encoder) {
 NSMutableDictionary *seen=objc_getAssociatedObject(encoder,&gProbeBindingsKey);
 if(!seen){seen=[NSMutableDictionary dictionary];objc_setAssociatedObject(encoder,&gProbeBindingsKey,seen,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}
 return seen;
}
static NSString *probeHex(const void *bytes,NSUInteger length,NSUInteger most) {
 NSMutableString *hex=[NSMutableString stringWithFormat:@"%lu:",(unsigned long)length];
 for(NSUInteger k=0;bytes && k<length && k<most;k++)[hex appendFormat:@"%s%02x",k%4?"":" ",((const uint8_t *)bytes)[k]];
 return hex;
}
static const char *probeStage(uint32_t stage) {return stage==1?"v":stage==2?"f":"c";}
static char gProbeWatchKey,gProbeVisKey,gProbeVisLabelsKey,gProbeDepthKey;
static NSString *probeBufferKind(id buffer) {
 NSMutableString *s=[NSMutableString stringWithFormat:@"len %lu storage %lu",(unsigned long)[(id<MTLBuffer>)buffer length],
  (unsigned long)[(id<MTLBuffer>)buffer storageMode]];
 SEL wrapped=NSSelectorFromString(@"wrapped");
 if([buffer respondsToSelector:wrapped] && ((void *(*)(id,SEL))objc_msgSend)(buffer,wrapped))[s appendString:@" no-copy"];
 if([(id<MTLBuffer>)buffer heap])[s appendString:@" heap"];
 return s;
}
// One binding of the probe's draw log, in pieces of 160 characters (bb36: the kernel log split longer lines and lost
// characters at each split): "replay N: key [i/n] piece".
static void probeLog(NSString *draw,NSString *key,id value) {
 NSString *text=[value description];
 const NSUInteger piece=160,count=MAX((NSUInteger)1,(text.length+piece-1)/piece);
 for(NSUInteger i=0;i<count;i++)
  NSLog(@"pandora-metal replay %@: %@ [%lu/%lu] %@",draw,key,(unsigned long)(i+1),(unsigned long)count,
        [text substringWithRange:NSMakeRange(i*piece,MIN(piece,text.length-i*piece))]);
}
#include <mach/mach.h>
#include "bridge/NativeMetalObjects.h"
#include "NativeBridgeMetalFrontend.h"
#include "PandoraBridgeTiming.h"   // data-a-live: every bridge call counted, 1 in 16 timed (tools/metal/adapt_ios16.py)

extern "C" void sys_dcache_flush(void *start, size_t length);

static NmbDevice *gDevice;
static id gDeviceObject;
static uint64_t gRegistryID;
static NSString *gName;
static void (*gMissing)(const char *, const char *);

// What this device has been asked and refused, for the plugin's status line (native_bridge_metal_status): commits, render/compute pipelines made,
// refusals, and the last two refusals' texts. Counters only; nothing here changes a result.
static std::atomic<uint64_t> gCommits,gCommitFailures,gPipelines,gProblems,gCompleted;
// What the render server actually asks of the GPU (int-141: 59 commits in 200 s, and a desktop that behaves like CPU compositing): render passes,
// draws, the pixels of their first colour attachment, compute dispatches, blits and the largest pass (pixels).
static std::atomic<uint64_t> gPasses,gDraws,gPassPixels,gDispatches,gBlits,gBiggestPass,gFullScreenPasses;
struct FormatWork {uint64_t passes=0,pixels=0;};
static std::mutex gFormatLock;
static std::map<uint64_t,FormatWork> gFormatWork;   // pixel format << 8 | load action << 4 | multisampled
// Where the time goes on the CPU side of the bridge: pipeline, library and function creation (count, total and slowest, in microseconds), and the
// last commits with what each held and how long the GPU took (commit to completion).
static uint64_t nowUs() {return clock_gettime_nsec_np(CLOCK_UPTIME_RAW)/1000;}
struct Timed {std::atomic<uint64_t> count{0},total{0},slowest{0};
 void add(uint64_t began){const uint64_t took=nowUs()-began;count++;total+=took;uint64_t seen=slowest.load();while(took>seen && !slowest.compare_exchange_weak(seen,took)){}}};
static Timed gTimePipeline,gTimeLibrary,gTimeFunction,gTimeEncodeToCommit;
static Timed gTimeWaitScheduled,gTimeWaitCompleted;
static Timed gTimeResourceWait;
static std::atomic<uint64_t> gResourceChecks,gResourceSkipped;
static Timed gTimeSurfaceFlush;
static std::atomic<uint64_t> gSurfaceFlushBytes,gSurfaceFlushDuplicates;
struct EncodedWork {uint32_t passes,draws,dispatches,blits;uint64_t pixels;};
struct CommitRecord {uint64_t committedUs=0,callbackUs=0,ageUs=0;int64_t scheduledUs=-1;uint32_t index=0;EncodedWork work={};};
static CommitRecord gCommitRing[32];
static std::mutex gCommitLock;
static std::mutex gProblemLock;
static char gLastProblems[2][200];
// dada: a line on the console as well (sandboxed Metal clients' os_log does not reach it; the kernel log is the only one a run keeps)
static const char *(*gLostReason)(void);   // Turnip's tu_darwin_lost_reason (native_bridge_metal_set_lost_reason)
extern "C" void native_bridge_metal_set_lost_reason(const char *(*reason)(void)) {gLostReason=reason;}
static void consoleLine(NSString *text) {
 static int fd=open("/dev/console",O_WRONLY|O_NOCTTY|O_CLOEXEC);
 if(fd<0)return;
 // dada: a lost device's reason, again with every 25th refusal it causes (printed once, it may be where the phone's log ring overwrites)
 static std::atomic<unsigned> lostLines;
 if(gLostReason && [text rangeOfString:@"DeviceLost"].location!=NSNotFound && lostLines++%25==0)
  text=[text stringByAppendingFormat:@" | Vulkan: %s",gLostReason()[0]?gLostReason():"(no loss recorded by Mesa)"];
 NSString *line=[NSString stringWithFormat:@"gpu2-metal[%s %d]: %@\n",getprogname(),getpid(),text];
 const char *bytes=line.UTF8String;if(bytes)(void)write(fd,bytes,strlen(bytes));
}
// The process's first problem, kept (UniXNU AdrenoMetal, 10-25): a client's first refusal is usually the cause of the rest. Every problem
// already reaches the console as it happens; this one is marked "first problem" as well, so a run's log names each process's root cause.
static char gFirstProblem[200];
static void noteProblem(NSString *what) {
 gProblems++;

 std::lock_guard<std::mutex> lock(gProblemLock);
 // A device answer the runtime lacks is reported at creation in every process and is harmless: the first problem is the next one.
 if(!gFirstProblem[0] && ![what hasPrefix:@"native-metal: no device answer"]) {
  strlcpy(gFirstProblem,what.UTF8String?:"?",sizeof gFirstProblem);
  consoleLine([@"first problem: " stringByAppendingString:what]);
 }
 memcpy(gLastProblems[1],gLastProblems[0],sizeof gLastProblems[0]);
 strlcpy(gLastProblems[0],what.UTF8String?:"?",sizeof gLastProblems[0]);
}
#define NBLOG(...) do{NSString *nbText=[NSString stringWithFormat:__VA_ARGS__];NSLog(@"%@",nbText);consoleLine(nbText);noteProblem(nbText);}while(0)
// data-a-live: the App Library capture (tools/metal/adapt_ios16.py).
static std::atomic<int> gCapture{0};
static char gCapNamesKey;
static inline bool capturing() {return gCapture.load(std::memory_order_relaxed)>0;}
static void capture(NSString *line) {if(gCapture.fetch_sub(1,std::memory_order_relaxed)>0)NSLog(@"pandora-metal cap %@",line);}
static NSString *capId(id object) {return [NSString stringWithFormat:@"%05lx",(unsigned long)(((uintptr_t)(__bridge void *)object>>4)&0xfffff)];}
static NSString *capTexture(id<MTLTexture> t) {
 if(!t)return @"-";
 id<MTLTexture> parent=[(id)t respondsToSelector:@selector(parentTexture)]?t.parentTexture:nil;
 const bool surface=[(id)t respondsToSelector:@selector(iosurface)] && t.iosurface;
 return [NSString stringWithFormat:@"%@ %lux%lu f%lu m%lu t%lu%@%@%@",capId(t),(unsigned long)t.width,(unsigned long)t.height,
  (unsigned long)t.pixelFormat,(unsigned long)t.mipmapLevelCount,(unsigned long)t.textureType,surface?@" surface":@"",
  parent?@" view-of ":@"",parent?capId(parent):@""];
}
// The pixel formats textures were asked for (first eight distinct), with how many were refused: native_bridge_metal_formats.
static uint32_t gFormats[8];static unsigned gFormatCount;static std::atomic<uint64_t> gTextureRefusals;
// Textures over IOSurfaces: made, refused, and how many of the requests named a planar surface.
static std::atomic<uint64_t> gSurfaceTextures,gSurfaceRefusals,gSurfacePlanar;
// The last refused surface texture: the check that refused it (1 plane, 2 base, 3 plane range, 4 import, 5 texture), the surface's format,
// planes, requested plane, plane offset, plane row bytes and allocation, and the descriptor's size and pixel format.
struct SurfaceRefusal {uint32_t step,format,planes,plane,pixelFormat;uint64_t offset,row,allocation,width,height;};
static SurfaceRefusal gSurfaceLastRefusal;
static void refuseSurface(uint32_t step,IOSurfaceRef surface,MTLTextureDescriptor *d,NSUInteger plane,uint64_t offset,uint64_t row) {
 gSurfaceRefusals++;
 SurfaceRefusal r={step,surface?IOSurfaceGetPixelFormat(surface):0,surface?(uint32_t)IOSurfaceGetPlaneCount(surface):0,(uint32_t)plane,
                   d?(uint32_t)d.pixelFormat:0,offset,row,surface?IOSurfaceGetAllocSize(surface):0,d?d.width:0,d?d.height:0};
 std::lock_guard<std::mutex> lock(gProblemLock);gSurfaceLastRefusal=r;
 static std::atomic<int> told{0};
 if(told.fetch_add(1)<40) {
  void *frames[8];const int n=backtrace(frames,8);std::string callers;char item[96];
  for(int i=1;i<n;i++){Dl_info info{};if(!dladdr(frames[i],&info) || !info.dli_fname)continue;const char *slash=strrchr(info.dli_fname,'/');
   snprintf(item,sizeof item," %s+%lx",slash?slash+1:info.dli_fname,(unsigned long)((uintptr_t)frames[i]-(uintptr_t)info.dli_fbase));callers+=item;}
  const uint32_t f=r.format;
  NSLog(@"pandora-metal surface refused: step %u '%c%c%c%c' planes %u plane %u pf %u %llux%llu row %llu offset %llu alloc %llu |%s",
        r.step,(char)(f>>24),(char)(f>>16),(char)(f>>8),(char)f,r.planes,r.plane,r.pixelFormat,(unsigned long long)r.width,
        (unsigned long long)r.height,(unsigned long long)r.row,(unsigned long long)r.offset,(unsigned long long)r.allocation,callers.c_str());
 }
}
static void noteFormat(uint32_t format,bool made) {
 std::lock_guard<std::mutex> lock(gProblemLock);
 if(!made)gTextureRefusals++;
 for(unsigned i=0;i<gFormatCount;i++)if(gFormats[i]==format)return;
 if(gFormatCount<8)gFormats[gFormatCount++]=format;
}
static NSError *nbError(NSString *why) {
 NSLog(@"pandora-metal refused: %@", why);   // data-a-live: every refusal in the log (backboardd: specialize, bb7)
 return [NSError errorWithDomain:@"org.blu-sharky.native-metal" code:-1 userInfo:@{NSLocalizedDescriptionKey:why?:@"refused"}];
}
static NSString *lastError() {return [NSString stringWithUTF8String:nmb_last_error()];}
// The render pipeline journal: one line per render pipeline creation (when, on which thread, how long, the outcome, both functions with
// every constant value, the attachments, blending and vertex descriptor) appended to pipelines.log in the runtime's cache directory, which
// the board log exports with the caches (CacheExport.h): every pipeline a boot made, or was refused, can be made again on the host.
static std::mutex gJournalLock;
static std::string gJournalPath;
static uint64_t gJournalBegan;
static void journal(const std::string &line) {
 if(gJournalPath.empty())return;
 std::lock_guard<std::mutex> lock(gJournalLock);
 if(FILE *f=fopen(gJournalPath.c_str(),"a")){fwrite(line.data(),1,line.size(),f);fputc('\n',f);fclose(f);}
}

@interface NBObject : NSObject
@property(atomic,copy) NSString *label;
@end
// dada (iOS 26): the selectors iOS 26's frameworks send to Metal objects that this file does not implement (NBDefaults.h, generated from
// the class-dumps of Apple's AGX driver, IOGPU and Metal against this file's classes: test/selaudit) are answered with a neutral value
// (0, nil, NO, a zeroed struct) and logged once each, instead of the exception that took SpringBoard down (g2vk3: -[NBTexture allocatedSize],
// -[NativeBridgeMetalDevice minimumTextureBufferAlignmentForPixelFormat:]).
#include "NBDefaults.h"
struct NBSize3 {uint64_t a,b,c;};
static unsigned __int128 nbDefaultInteger(id,SEL) {return 0;}
static double nbDefaultFloat(id,SEL) {return 0;}
static NBSize3 nbDefaultSize3(id,SEL) {return NBSize3{};}
static bool nbAddDefault(Class cls,SEL sel) {
 static std::unordered_map<std::string,unsigned char> *table=[]{
  auto *t=new std::unordered_map<std::string,unsigned char>;for(const NBDefault &d:kNBDefaults)(*t)[d.sel]=d.kind;return t;}();
 auto at=table->find(sel_getName(sel));
 if(at==table->end())return false;
 IMP imp=at->second==1?(IMP)nbDefaultFloat:at->second==2?(IMP)nbDefaultSize3:(IMP)nbDefaultInteger;
 const char *types=at->second==1?"d16@0:8":at->second==2?"{NBSize3=QQQ}16@0:8":"Q16@0:8";
 if(!class_addMethod(cls,sel,imp,types))return false;
 NSLog(@"gpu2-metal: -[%s %s] answered with the neutral default",class_getName(cls),sel_getName(sel));
 consoleLine([NSString stringWithFormat:@"-[%s %s] answered with the neutral default",class_getName(cls),sel_getName(sel)]);
 return true;
}
@implementation NBObject
+ (BOOL)resolveInstanceMethod:(SEL)sel {return nbAddDefault(self,sel)||[super resolveInstanceMethod:sel];}
- (id<MTLDevice>)device {return gDeviceObject;}
// Identity SPI every Metal object has on iOS 26 (Core Image, QuartzCore, PosterBoard key caches on them): this object's address, which is
// unique while it lives, never 0.
- (uint64_t)uniqueIdentifier {return (uint64_t)(uintptr_t)(__bridge void *)self;}
- (MTLResourceID)gpuResourceID {MTLResourceID id;memset(&id,0,sizeof id);uint64_t v=(uint64_t)(uintptr_t)(__bridge void *)self;memcpy(&id,&v,sizeof v);return id;}
- (BOOL)isComplete {return YES;}
- (void)doesNotRecognizeSelector:(SEL)selector {
 if(gMissing)gMissing(class_getName(object_getClass(self)),sel_getName(selector));
 noteProblem([NSString stringWithFormat:@"missing %s %s",class_getName(object_getClass(self)),sel_getName(selector)]);
 [super doesNotRecognizeSelector:selector];
}
@end

// ---- resources ----
// The wait takes a strong snapshot of already-committed buffers; new commits require the
// caller's usual resource synchronization. Storage dependency checks are in the runtime.
static NSMutableSet *gInFlight;
static void waitForResource(id resource);
// Resource SPI the runtime has no state for: no protected content (Metal's default options), and no owner transfer
// (memory attribution is the kernel driver's, by the importing task), refused as not supported.
#define NB_RESOURCE_SPI \
- (uint64_t)protectionOptions {return 0;} \
- (id<MTLHeap>)heap {return _parentHeap;} \
- (int)setOwnerWithIdentity:(unsigned)identity {(void)identity;return 46; /* KERN_NOT_SUPPORTED */} \
- (void)setResponsibleProcess:(int)pid {(void)pid; /* iOS/macOS 26 memory attribution: the importing task's, as above */} \
- (void)waitUntilComplete {waitForResource(self);}
@interface NBBuffer : NBObject
@property(nonatomic) NmbBuffer *handle;
@property(nonatomic) MTLResourceOptions options;
@property(nonatomic) void *wrapped;
// dada: a no-copy buffer over memory the kernel cannot import (RedwoodGPU imports IOSurfaces only): a buffer of the GPU's own that
// mirrors the caller's bytes, copied in at every commit (syncMirrors); contents is the caller's pointer, as Metal's is.
@property(nonatomic) void *mirror;
@property(nonatomic) NSUInteger mirrorLength;   // the caller's bytes (the GPU's buffer may be rounded up past them)
@property(nonatomic,copy) void (^mirrorRelease)(void *,NSUInteger);
@property(nonatomic,strong) id<MTLHeap> parentHeap;   // the heap that made the buffer, nil otherwise
@property(nonatomic) NSUInteger heapOffset;
@end
@interface NBTexture : NBObject
@property(nonatomic) NmbTexture *handle;
@property(nonatomic) NmbTextureDescriptor descriptor;
@property(nonatomic,strong) NBBuffer *buffer;
@property(nonatomic) IOSurfaceRef surface;
@property(nonatomic) NSUInteger offset,rowBytes;
@property(nonatomic,strong) id<MTLTexture> parent;
@property(nonatomic,strong) id<MTLHeap> parentHeap;
@property(nonatomic) NSUInteger heapOffset;
@end
// A no-copy import keeps its pages until the runtime frees it, which can be after the buffer object goes (a command buffer still using
// it). The owner's release (Metal's deallocator, an IOSurface's mapping) waits for that, as Metal's does: run earlier, the owner could
// unmap or reuse pages the GPU still reads or writes.
static std::atomic<uint64_t> gNoCopyAll,gNoCopyMade,gNoCopyReleased,gSurfaceImportsMade,gSurfaceImportsReleased;
static void releaseBlock(void *context,void *pointer,uint64_t length) {
 gNoCopyReleased++;
 void (^deallocator)(void *,NSUInteger)=(__bridge_transfer void (^)(void *,NSUInteger))context;deallocator(pointer,(NSUInteger)length);
}
static void releaseSurface(void *context,void *,uint64_t) {gSurfaceImportsReleased++;CFRelease((IOSurfaceRef)context);}
static std::mutex gMirrorLock,gMirrorSyncLock;
static NSHashTable *gMirrors;   // the live mirrored no-copy buffers (weak)
static std::atomic<uint64_t> gMirrorBytesSynced,gMirrorBuffersCopied;
static Timed gTimeMirrorSync;
static std::atomic<uint64_t> gMirrorSnapshotBytes,gMirrorSnapshotFallbacks;
static std::atomic<uint64_t> gMirrorParallelBatches,gMirrorParallelBuffers;
// The caller's bytes into every mirror, before a command buffer's work can read them.
// The caller may give its memory back while the buffer still lives (a command buffer in flight holds it): a plain copy then read
// unmapped pages and took the render server down (g2vk16: SIGSEGV in this copy, every ten seconds). The kernel's copy refuses instead,
// and that buffer is mirrored no more.
static void syncMirrors() {
 const uint64_t began=nowUs();
 std::lock_guard<std::mutex> sync(gMirrorSyncLock);
 NSArray<NBBuffer *> *candidates;
 {std::lock_guard<std::mutex> lock(gMirrorLock);candidates=gMirrors.allObjects;}
 // Every mirror remains retained and is refreshed before submission. Split
 // independent allocations across three workers rather than making the render
 // thread copy hundreds of megabytes serially. Never filter by resource usage.
 NSMutableArray *gone=[NSMutableArray array];
 std::mutex goneLock;
 auto copyOne=[&](NBBuffer *b) {
  void *gpu=nmb_buffer_contents(b.handle);const uint64_t length=b.mirrorLength;
  if(!gpu||!b.mirror||!length)return;
  bool copiedAll=false;
  // A kernel-owned snapshot keeps the source pages readable even if the
  // caller unmaps them. Large copies then use libc's optimized memcpy into
  // the GPU mapping. Nothing is cached between commands or omitted.
  if(length>=(1u<<20) && length<=UINT32_MAX) {
   vm_offset_t snapshot=0;mach_msg_type_number_t snapshotBytes=0;
   const kern_return_t kr=vm_read(mach_task_self(),(vm_address_t)b.mirror,
       (vm_size_t)length,&snapshot,&snapshotBytes);
   if(kr==KERN_SUCCESS && snapshot && snapshotBytes==length) {
    memcpy(gpu,(const void *)(uintptr_t)snapshot,(size_t)length);
    sys_dcache_flush(gpu,(size_t)length);
    gMirrorSnapshotBytes+=length;copiedAll=true;
   }
   if(kr==KERN_SUCCESS && snapshot)vm_deallocate(mach_task_self(),(vm_address_t)snapshot,(vm_size_t)snapshotBytes);
   if(!copiedAll)gMirrorSnapshotFallbacks++;
  }
  if(!copiedAll) {
   vm_size_t copied=0;
   copiedAll=vm_read_overwrite(mach_task_self(),(vm_address_t)b.mirror,(vm_size_t)length,(vm_address_t)gpu,&copied)==KERN_SUCCESS && copied==length;
  }
  if(copiedAll){gMirrorBytesSynced+=length;gMirrorBuffersCopied++;}
  else{std::lock_guard<std::mutex> lock(goneLock);[gone addObject:b];}
 };
 uint64_t totalBytes=0;
 for(NBBuffer *b in candidates)totalBytes+=b.mirrorLength;
 const NSUInteger workers=totalBytes>=(4u<<20) && candidates.count>=3 ? 3 : 1;
 if(workers>1){
  gMirrorParallelBatches++;gMirrorParallelBuffers+=candidates.count;
  dispatch_apply(workers,dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^(size_t worker){
   @autoreleasepool {
    for(NSUInteger i=worker;i<candidates.count;i+=workers)copyOne(candidates[i]);
   }
  });
 } else for(NBBuffer *b in candidates)copyOne(b);
 for(NBBuffer *b in gone){
  {std::lock_guard<std::mutex> lock(gMirrorLock);[gMirrors removeObject:b];}
  static std::atomic<unsigned> said;if(said++<5)consoleLine([NSString stringWithFormat:@"mirrored no-copy buffer of %lu bytes: its memory is gone; not copied again",(unsigned long)b.mirrorLength]);
 }
 gTimeMirrorSync.add(began);
}

@implementation NBBuffer
- (void)dealloc {
 if(_mirror){std::lock_guard<std::mutex> lock(gMirrorLock);[gMirrors removeObject:self];}
 if(_mirror&&_mirrorRelease)_mirrorRelease(_mirror,_mirrorLength);
 nmb_buffer_release(_handle);
}
- (NSUInteger)length {return _mirror?_mirrorLength:nmb_buffer_length(_handle);}
- (void *)contents {return _mirror?_mirror:nmb_buffer_contents(_handle);}
- (uint64_t)gpuAddress {return nmb_buffer_gpu_address(_handle);}
- (MTLResourceOptions)resourceOptions {return _options;}
- (MTLStorageMode)storageMode {return (MTLStorageMode)((_options&MTLResourceStorageModeMask)>>MTLResourceStorageModeShift);}
- (MTLCPUCacheMode)cpuCacheMode {return (MTLCPUCacheMode)((_options&MTLResourceCPUCacheModeMask)>>MTLResourceCPUCacheModeShift);}
- (MTLHazardTrackingMode)hazardTrackingMode {return (MTLHazardTrackingMode)((_options&MTLResourceHazardTrackingModeMask)>>MTLResourceHazardTrackingModeShift);}
- (void)didModifyRange:(NSRange)range {
 if(_mirror&&range.location<_mirrorLength){
  const NSUInteger n=MIN(range.length,_mirrorLength-range.location);
  vm_size_t copied=0;   // the kernel's copy, as in syncMirrors: the caller's memory may be gone
  (void)vm_read_overwrite(mach_task_self(),(vm_address_t)((uint8_t *)_mirror+range.location),(vm_size_t)n,
                          (vm_address_t)((uint8_t *)nmb_buffer_contents(_handle)+range.location),&copied);
 }
}
- (void)makeAliasable {if(!nmb_buffer_make_aliasable(_handle))NBLOG(@"native-metal: buffer makeAliasable refused: %@",lastError());}
- (BOOL)isAliasable {return nmb_buffer_is_aliasable(_handle);}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {
 const uint64_t answer=nmb_buffer_set_purgeable_state(_handle,state);
 if(!answer)NBLOG(@"native-metal: buffer setPurgeableState refused: %@",lastError());
 return (MTLPurgeableState)answer;
}
- (NSUInteger)allocatedSize {return nmb_buffer_length(_handle);}
- (void *)virtualAddress {return nmb_buffer_contents(_handle);}   // dada: iOS 26 IOSurface SPI
- (NSUInteger)bufferOffset {return 0;}
- (IOSurfaceRef)iosurface {return nullptr;}   // a buffer is never the backing of a surface here (surfaces back textures)
NB_RESOURCE_SPI
@end
// Reuse only an import still held by a texture/view or pending command. Weak
// values never extend a GPU allocation's lifetime. The strong buffer snapshot
// and its retained surface prevent object/address reuse during a hit. A new
// mapping, allocation identity or size needs a separate import; ordinary
// caller-owned no-copy buffers do not enter this cache.
static NSMapTable<NSValue *,NBBuffer *> *gSurfaceImports;
static std::mutex gSurfaceImportLock;
// dada: Turnip's tu_darwin_note_surface, which its library (loaded RTLD_LOCAL, so not in the global namespace) exports; the plugin
// looks it up in that library and hands it here (native_bridge_metal_set_surface_note).
static void (*gSurfaceNote)(uint32_t,const void *,uint64_t);
extern "C" void native_bridge_metal_set_surface_note(void (*note)(uint32_t,const void *,uint64_t)) {gSurfaceNote=note;}
static NBBuffer *surfaceBuffer(IOSurfaceRef surface,void *base,size_t bytes) {
 struct Key {uint64_t identity,address,bytes;};
 const Key value={IOSurfaceGetID(surface),(uintptr_t)base,bytes};
 NSValue *key=value.identity?[NSValue valueWithBytes:&value objCType:@encode(Key)]:nil;
 std::lock_guard<std::mutex> lock(gSurfaceImportLock);
 if(!gSurfaceImports)gSurfaceImports=[NSMapTable strongToWeakObjectsMapTable];
 NBBuffer *buffer=key?[gSurfaceImports objectForKey:key]:nil;
 // dada: the recent imports are held as well (gSurfaceRecent): a surface's texture is made and released every frame, and each new import
 // cost 2 to 7 ms (g2vk17: 512 imports made, 520 released in 30 s; the frame rate 5 to 17 a second).
 static NSMutableArray<NBBuffer *> *recent;
 static uint64_t recentBytes;
 auto keep=[&](NBBuffer *b,uint64_t n){
  if(!recent)recent=[NSMutableArray array];
  const NSUInteger at=[recent indexOfObjectIdenticalTo:b];
  if(at!=NSNotFound)[recent removeObjectAtIndex:at];else recentBytes+=n;
  [recent addObject:b];
  // 6 and 64 MB at most: RedwoodGPU has one 3.5 GB GPU address space for every process, and holding 16 imports (192 MB) in each
  // of several processes helped fill it (g2vk22: 4.6 to 8 MB allocations refused, kIOReturnNoMemory, in posters and widgets)
  while(recent.count>6||recentBytes>(64ull<<20)){
   recentBytes-=MIN(recentBytes,(uint64_t)nmb_buffer_length(recent[0].handle));[recent removeObjectAtIndex:0];
  }
 };
 if(buffer){keep(buffer,bytes);return buffer;}
 // dada: the kernel imports an IOSurface by its ID, not a range of pages; Turnip's RedwoodGPU transport learns which surface these are.
 if(gSurfaceNote)gSurfaceNote(IOSurfaceGetID(surface),base,bytes);
 else{static bool said;if(!said){said=true;consoleLine(@"no surface hook: Turnip cannot import surfaces");}}
 // The surface keeps its CPU mapping (the imported pages) until the import is freed.
 NmbBuffer *handle=nmb_buffer_new_no_copy_releasing(gDevice,base,bytes,MTLResourceStorageModeShared,releaseSurface,(void *)CFRetain(surface));
 if(!handle){CFRelease(surface);NBLOG(@"native-metal: surface import refused: %@",lastError());return nil;}
 gSurfaceImportsMade++;buffer=[NBBuffer new];buffer.handle=handle;buffer.options=MTLResourceStorageModeShared;
 if(key) {
  // Bound metadata without retaining old frames. Evicting an association is
  // harmless: its owners still retain the actual import until their own end.
  if(gSurfaceImports.count>=256) {
   for(NSValue *old in gSurfaceImports.keyEnumerator.allObjects)
    if(![gSurfaceImports objectForKey:old])[gSurfaceImports removeObjectForKey:old];
   if(gSurfaceImports.count>=256)[gSurfaceImports removeObjectForKey:gSurfaceImports.keyEnumerator.nextObject];
  }
  [gSurfaceImports setObject:buffer forKey:key];
  keep(buffer,bytes);
 }
 return buffer;
}
// dada: NB_SINGLE_SAMPLE=1 (the plug-in sets it for the posters): multisampled textures, pipelines and passes made single-sampled, a
// resolving pass drawing straight into its resolve texture. The lock screen's wallpaper drew 4x MSAA frames of 135-195 ms of GPU each
// (g2k31), and the one ring made the render server's frames wait behind them: the stutter. Its edges lose their antialiasing.
// (read on first use: the plug-in sets the variable as the device opens, after this file's statics ran)
static bool singleSample() {static const bool on=[]{const char *v=getenv("NB_SINGLE_SAMPLE");return v && *v=='1';}();return on;}
#define gSingleSample singleSample()
// dada: MTLTextureSwizzleChannels as NmbTextureDescriptor's packed word (a channel a byte, red lowest), 0 for the identity.
static uint64_t packSwizzle(MTLTextureSwizzleChannels s) {
 if(s.red==MTLTextureSwizzleRed && s.green==MTLTextureSwizzleGreen && s.blue==MTLTextureSwizzleBlue && s.alpha==MTLTextureSwizzleAlpha)return 0;
 return (uint64_t)s.red|(uint64_t)s.green<<8|(uint64_t)s.blue<<16|(uint64_t)s.alpha<<24;
}
static MTLTextureSwizzleChannels unpackSwizzle(uint64_t p) {
 if(!p)return MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed,MTLTextureSwizzleGreen,MTLTextureSwizzleBlue,MTLTextureSwizzleAlpha);
 return MTLTextureSwizzleChannelsMake((MTLTextureSwizzle)(p&0xff),(MTLTextureSwizzle)(p>>8&0xff),(MTLTextureSwizzle)(p>>16&0xff),(MTLTextureSwizzle)(p>>24&0xff));
}
static bool textureDescriptor(MTLTextureDescriptor *d,NmbTextureDescriptor *out) {
 if(!d)return false;
 *out={(uint64_t)d.textureType,(uint64_t)d.pixelFormat,d.width,d.height,d.depth,d.mipmapLevelCount,d.sampleCount,d.arrayLength,(uint64_t)d.usage,(uint64_t)d.resourceOptions,0};
 out->swizzle=packSwizzle(d.swizzle);
 if(gSingleSample && d.sampleCount>1) {
  out->samples=1;
  if(d.textureType==MTLTextureType2DMultisample)out->type=MTLTextureType2D;
  if(d.textureType==MTLTextureType2DMultisampleArray)out->type=MTLTextureType2DArray;
 }
 return true;
}
// dada: the render server's screen-sized textures (a million pixels and more) as Metal objects: how many live, are views, are set
// volatile by their owner, and made and freed since the last report (g2k8: backboardd's memory of them grew 110 MB every 5 s until
// the phone froze), in the bridge timing report.
static std::atomic<int64_t> gBigLive,gBigViews,gBigVolatile,gBigMade,gBigFreed,gBigBytes;
// Who makes the live ones: the caller (first frame outside this plugin and Metal) of each, counted while it lives.
static std::mutex gBigLock;
static std::unordered_map<std::string,int64_t> *gBigCallers;
static std::string bigCaller() {
 void *frames[10];const int n=backtrace(frames,10);std::string out;char item[96];int kept=0;
 for(int i=2;i<n && kept<3;i++){Dl_info info{};if(!dladdr(frames[i],&info) || !info.dli_fname)continue;const char *slash=strrchr(info.dli_fname,'/');
  const char *name=slash?slash+1:info.dli_fname;if(!strcmp(name,"RedwoodMetal") || !strcmp(name,"Metal"))continue;
  snprintf(item,sizeof item,"%s%s+%lx",kept?" ":"",name,(unsigned long)((uintptr_t)frames[i]-(uintptr_t)info.dli_fbase));out+=item;kept++;}
 return out;
}
@implementation NBTexture {bool _big;MTLPurgeableState _purge;std::string _maker;}
- (void)setDescriptor:(NmbTextureDescriptor)d {
 _descriptor=d;
 if(!_big && d.width*d.height>=1000000){
  _big=true;gBigLive++;gBigMade++;gBigBytes+=(int64_t)(d.width*d.height*4);
  _maker=bigCaller()+(_surface?" (surface)":"");
  std::lock_guard<std::mutex> lock(gBigLock);if(!gBigCallers)gBigCallers=new std::unordered_map<std::string,int64_t>;(*gBigCallers)[_maker]++;
 }
}
- (void)setParent:(id<MTLTexture>)parent {
 if(_big && parent && !_parent)gBigViews++;
 if(_big && !parent && _parent)gBigViews--;
 _parent=parent;
}
// iOS/macOS 26's QuartzCore reads these of every texture it composites. dada: the swizzle the texture was made or viewed with (an
// opaque layer's surface is read through a view whose alpha is One; before 29 October none was, and the wallpaper looked overexposed).
- (MTLTextureSwizzleChannels)swizzle {return unpackSwizzle(self.descriptor.swizzle);}
- (BOOL)isDrawable {return NO;}
// iOS 26's QuartzCore and Core Image read these of textures (dada, NBDefaults.h covers the rest). The size is the texels' bytes at most 8 per
// texel (the runtime keeps the real layout), a third more with mipmaps; a view or a buffer's texture owns nothing of its own.
- (NSUInteger)allocatedSize {
 if(_parent||_buffer||_surface)return 0;
 const NmbTextureDescriptor &d=_descriptor;
 uint64_t bytes=d.width*MAX(d.height,1ull)*MAX(d.depth,1ull)*MAX(d.array_length,1ull)*MAX(d.samples,1ull)*8;
 return (NSUInteger)(d.levels>1?bytes+bytes/3:bytes);
}
- (id<MTLResource>)rootResource {return _parent?[(id)_parent rootResource]:_buffer?(id<MTLResource>)_buffer:(id<MTLResource>)self;}
- (BOOL)allowGPUOptimizedContents {return YES;}
- (MTLHazardTrackingMode)hazardTrackingMode {
 return (MTLHazardTrackingMode)((_descriptor.options&MTLResourceHazardTrackingModeMask)>>MTLResourceHazardTrackingModeShift);
}
- (void *)virtualAddress {return _buffer?(uint8_t *)[_buffer contents]+_offset:nullptr;}
- (void)dealloc {
 if(_big){gBigLive--;gBigFreed++;gBigBytes-=(int64_t)(_descriptor.width*_descriptor.height*4);if(_parent)gBigViews--;if(_purge==MTLPurgeableStateVolatile)gBigVolatile--;
  std::lock_guard<std::mutex> lock(gBigLock);if(gBigCallers && --(*gBigCallers)[_maker]<=0)gBigCallers->erase(_maker);}
 nmb_texture_release(_handle);if(_surface)CFRelease(_surface);
}
- (MTLTextureType)textureType {return (MTLTextureType)_descriptor.type;}
- (MTLPixelFormat)pixelFormat {return (MTLPixelFormat)_descriptor.format;}
- (NSUInteger)width {return _descriptor.width;}
- (NSUInteger)height {return _descriptor.height;}
- (NSUInteger)depth {return _descriptor.depth;}
- (NSUInteger)mipmapLevelCount {return _descriptor.levels;}
- (NSUInteger)sampleCount {return _descriptor.samples;}
- (NSUInteger)arrayLength {return _descriptor.array_length;}
- (MTLTextureUsage)usage {return (MTLTextureUsage)_descriptor.usage;}
- (MTLResourceOptions)resourceOptions {return (MTLResourceOptions)_descriptor.options;}
- (MTLStorageMode)storageMode {return (MTLStorageMode)((_descriptor.options&MTLResourceStorageModeMask)>>MTLResourceStorageModeShift);}
- (MTLCPUCacheMode)cpuCacheMode {return (MTLCPUCacheMode)((_descriptor.options&MTLResourceCPUCacheModeMask)>>MTLResourceCPUCacheModeShift);}
- (BOOL)isFramebufferOnly {return NO;}
- (IOSurfaceRef)iosurface {return _surface;}
- (NSUInteger)iosurfacePlane {return 0;}
- (id<MTLBuffer>)buffer {return _surface?nil:(id<MTLBuffer>)_buffer;}
- (NSUInteger)bufferOffset {return _offset;}
- (NSUInteger)bufferBytesPerRow {return _rowBytes;}
- (id<MTLTexture>)parentTexture {return _parent;}
- (NSUInteger)rotation {return 0;}
// dada: a neutral default before (nothing done): CoreAnimation's heaps never got a texture's range back and filled up (g2k11).
- (void)makeAliasable {if(!_parent && !nmb_texture_make_aliasable(_handle))NBLOG(@"native-metal: texture makeAliasable refused: %@",lastError());}
- (BOOL)isAliasable {return !_parent && nmb_texture_is_aliasable(_handle);}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {
 const uint64_t answer=nmb_texture_set_purgeable_state(_handle,state);
 if(!answer)NBLOG(@"native-metal: texture setPurgeableState refused: %@",lastError());
 if(_big && state!=MTLPurgeableStateKeepCurrent){
  if(_purge==MTLPurgeableStateVolatile && state!=MTLPurgeableStateVolatile)gBigVolatile--;
  if(_purge!=MTLPurgeableStateVolatile && state==MTLPurgeableStateVolatile)gBigVolatile++;
  _purge=state;
 }
 return (MTLPurgeableState)answer;
}
NB_RESOURCE_SPI
- (id<MTLTexture>)view:(NmbTexture *)handle {
 if(!handle){NBLOG(@"native-metal: texture view refused: %@",lastError());return nil;}
 NBTexture *view=[NBTexture new];view.handle=handle;NmbTextureDescriptor descriptor;nmb_texture_describe(handle,&descriptor);view.descriptor=descriptor;
 view.parent=(id<MTLTexture>)self;view.buffer=_buffer;view.offset=_offset;view.rowBytes=_rowBytes;
 if(_surface)view.surface=(IOSurfaceRef)CFRetain(_surface);
 return (id<MTLTexture>)view;
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format {
 return [self view:nmb_texture_new_view(_handle,format,0,nullptr,nullptr,true)];
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format textureType:(MTLTextureType)type levels:(NSRange)levels slices:(NSRange)slices {
 const uint64_t l[2]={levels.location,levels.length},s[2]={slices.location,slices.length};
 return [self view:nmb_texture_new_view(_handle,format,type,l,s,false)];
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format textureType:(MTLTextureType)type levels:(NSRange)levels slices:(NSRange)slices
                                        swizzle:(MTLTextureSwizzleChannels)swizzle {
 const uint64_t l[2]={levels.location,levels.length},s[2]={slices.location,slices.length};
 static std::atomic<int> told{0};
 if(told++<4)consoleLine([NSString stringWithFormat:@"a texture view swizzled %u/%u/%u/%u (pf %lu)",(unsigned)swizzle.red,(unsigned)swizzle.green,
                          (unsigned)swizzle.blue,(unsigned)swizzle.alpha,(unsigned long)format]);
 return [self view:nmb_texture_new_view_swizzle(_handle,format,type,l,s,packSwizzle(swizzle))];
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice withBytes:(const void *)bytes bytesPerRow:(NSUInteger)row bytesPerImage:(NSUInteger)image {
 const uint64_t region[6]={r.origin.x,r.origin.y,r.origin.z,r.size.width,r.size.height,r.size.depth};
 const uint64_t length=(r.size.depth>1?image*(r.size.depth-1):0)+row*r.size.height;
 if(!nmb_texture_replace(_handle,region,level,slice,bytes,row,image,length))NBLOG(@"native-metal: replaceRegion refused: %@",lastError());
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level withBytes:(const void *)bytes bytesPerRow:(NSUInteger)row {
 [self replaceRegion:r mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:row bytesPerImage:row*r.size.height];
}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)row bytesPerImage:(NSUInteger)image fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice {
 const uint64_t region[6]={r.origin.x,r.origin.y,r.origin.z,r.size.width,r.size.height,r.size.depth};
 const uint64_t length=(r.size.depth>1?image*(r.size.depth-1):0)+row*r.size.height;
 if(!nmb_texture_get_bytes(_handle,bytes,row,image,length,region,level,slice))NBLOG(@"native-metal: getBytes refused: %@",lastError());
}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)row fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level {
 [self getBytes:bytes bytesPerRow:row bytesPerImage:row*r.size.height fromRegion:r mipmapLevel:level slice:0];
}
@end
// A command can use many independent imports/views of one IOSurface. Their
// owners keep the mapping live through this phase. Maintain each exact CPU
// range once; no address identity or decision is carried into another phase
// or command, and distinct/partially overlapping ranges retain their calls.
static void flushSurfaces(NSMutableSet *surfaces) {
 if(!surfaces.count)return;
 std::vector<std::pair<uintptr_t,size_t>> ranges;
 ranges.reserve(surfaces.count);
 for(NBTexture *texture in surfaces)if(texture.surface)
  ranges.emplace_back(reinterpret_cast<uintptr_t>(IOSurfaceGetBaseAddress(texture.surface)),IOSurfaceGetAllocSize(texture.surface));
 std::sort(ranges.begin(),ranges.end());
 const auto last=std::unique(ranges.begin(),ranges.end());
 gSurfaceFlushDuplicates+=(uint64_t)(ranges.end()-last);
 for(auto it=ranges.begin();it!=last;++it) {
  const uint64_t began=nowUs();
  sys_dcache_flush(reinterpret_cast<void *>(it->first),it->second);
  gSurfaceFlushBytes+=it->second;gTimeSurfaceFlush.add(began);
 }
}
// data-a-live: no-copy buffers a command uses, directly or under a texture (tools/metal/adapt_ios16.py).
static std::atomic<uint64_t> gWrappedFlushes{0},gWrappedFlushBytes{0};
static void flushWrapped(NSSet *owners) {
 if(!owners.count)return;
 std::vector<std::pair<uintptr_t,size_t>> ranges;
 for(id o in owners) {
  NBBuffer *b=nil;
  if([o isKindOfClass:[NBBuffer class]])b=o;
  else if([o isKindOfClass:[NBTexture class]])b=((NBTexture *)o).buffer;
  if(b && b.wrapped)ranges.emplace_back(reinterpret_cast<uintptr_t>(b.wrapped),(size_t)[(id<MTLBuffer>)b length]);
 }
 if(ranges.empty())return;
 std::sort(ranges.begin(),ranges.end());
 const auto last=std::unique(ranges.begin(),ranges.end());
 for(auto it=ranges.begin();it!=last;++it){sys_dcache_flush(reinterpret_cast<void *>(it->first),it->second);gWrappedFlushBytes+=it->second;}
 const uint64_t n=++gWrappedFlushes;
 if(n<=3 || n%2000==0)NSLog(@"pandora-metal wrapped: %llu cleanings, %llu MiB in all",(unsigned long long)n,(unsigned long long)(gWrappedFlushBytes.load()>>20));
}
static NBTexture *textureOnBuffer(NBBuffer *buffer,MTLTextureDescriptor *d,NSUInteger offset,NSUInteger row,bool surface) {
 NmbTextureDescriptor descriptor;if(!buffer || !textureDescriptor(d,&descriptor))return nil;
 NmbTexture *handle=nmb_texture_new_on_buffer(buffer.handle,&descriptor,offset,row,surface);
 if(!handle){NBLOG(@"native-metal: texture over buffer refused: %@",lastError());return nil;}
 NBTexture *texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;
 texture.buffer=buffer;texture.offset=offset;texture.rowBytes=row;return texture;
}
// A texture over a two-plane surface: the runtime reads plane 1 from the surface's own layout (two-plane RGB and YCbCr formats).
// data-a-live: a 'b3a8' surface's planes when it reports none (bb43): {offset0, row0, offset1, row1}, or false.
static bool pandoraB3a8Planes(IOSurfaceRef surface,size_t allocation,size_t planes[4]) {
 if(!surface || IOSurfaceGetPixelFormat(surface)!=0x62336138 || IOSurfaceGetPlaneCount(surface)!=2)return false;
 if(IOSurfaceGetBytesPerRowOfPlane(surface,0) && IOSurfaceGetBytesPerRowOfPlane(surface,1))return false;
 const size_t w=IOSurfaceGetWidth(surface),h=IOSurfaceGetHeight(surface);
 const size_t row0=(w*4+63)&~(size_t)63,row1=(w+63)&~(size_t)63;
 if(!w || !h || row0*h+row1*h>allocation)return false;
 planes[0]=0;planes[1]=row0;planes[2]=row0*h;planes[3]=row1;
 static std::atomic<int> told{0};
 if(told.fetch_add(1)<8)NSLog(@"pandora-metal b3a8 %lux%lu: planes computed (row %lu at 0, row %lu at %lu; %lu of %lu bytes)",(unsigned long)w,
  (unsigned long)h,(unsigned long)row0,(unsigned long)row1,(unsigned long)(row0*h),(unsigned long)(row0*h+row1*h),(unsigned long)allocation);
 return true;
}
static NBTexture *textureOnSurface(NBBuffer *buffer,MTLTextureDescriptor *d,IOSurfaceRef surface,NSUInteger plane,NSUInteger offset,NSUInteger row) {
 NmbTextureDescriptor descriptor;if(!buffer || !textureDescriptor(d,&descriptor))return nil;
 const uint64_t base=(uintptr_t)IOSurfaceGetBaseAddress(surface),chroma=(uintptr_t)IOSurfaceGetBaseAddressOfPlane(surface,1);
 uint64_t chromaOffset=chroma>=base?chroma-base:0,chromaRow=IOSurfaceGetBytesPerRowOfPlane(surface,1);
 size_t computed[4];   // data-a-live: a 'b3a8' surface without plane rows (bb43)
 if(!chromaRow && pandoraB3a8Planes(surface,IOSurfaceGetAllocSize(surface),computed)){chromaOffset=computed[2];chromaRow=computed[3];}
 NmbTexture *handle=nmb_texture_new_on_surface(buffer.handle,&descriptor,offset,row,IOSurfaceGetPixelFormat(surface),plane,
  IOSurfaceGetWidth(surface),IOSurfaceGetHeight(surface),chromaOffset,chromaRow);
 if(!handle){NBLOG(@"native-metal: texture over surface refused: %@",lastError());return nil;}
 NBTexture *texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;
 texture.buffer=buffer;texture.offset=offset;texture.rowBytes=row;return texture;
}
@implementation NBBuffer (Textures)
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset bytesPerRow:(NSUInteger)row {
 return (id<MTLTexture>)textureOnBuffer(self,d,offset,row,false);
}
- (id<MTLTexture>)newLinearTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset bytesPerRow:(NSUInteger)row bytesPerImage:(NSUInteger)image {
 NmbTextureDescriptor descriptor;if(!textureDescriptor(d,&descriptor))return nil;
 NmbTexture *handle=nmb_texture_new_linear(self.handle,&descriptor,offset,row,image);
 if(!handle){NBLOG(@"native-metal: linear texture refused: %@",lastError());return nil;}
 NBTexture *texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;
 texture.buffer=self;texture.offset=offset;texture.rowBytes=row;return (id<MTLTexture>)texture;
}
@end

// ---- libraries, functions, states ----
@interface NBLibrary : NBObject
@property(nonatomic) NmbLibrary *handle;
@end
@interface NBFunction : NBObject
@property(nonatomic) NmbFunction *handle;
@property(nonatomic,copy) NSString *name;
@property(nonatomic) MTLFunctionType functionType;
@property(nonatomic) std::string constantsText;   // name:type:hex of each constant value it was specialized with (the journal's)
@end
// dada: functionConstantsDictionary's values (MTLFunctionConstant is a class with read-only properties and no public maker)
@interface NBFunctionConstant : MTLFunctionConstant
@property(nonatomic,copy) NSString *nbName;
@property(nonatomic) MTLDataType nbType;
@property(nonatomic) NSUInteger nbIndex;
@property(nonatomic) BOOL nbRequired;
@end
@implementation NBFunctionConstant
- (NSString *)name {return _nbName;}
- (MTLDataType)type {return _nbType;}
- (NSUInteger)index {return _nbIndex;}
- (BOOL)required {return _nbRequired;}
@end
@implementation NBFunction
- (void)dealloc {nmb_function_release(_handle);}
// dada: answered nil by a neutral default before: VFX (WeatherPoster) supplies the constants this names, and supplied none (g2k25)
- (NSDictionary<NSString *,MTLFunctionConstant *> *)functionConstantsDictionary {
 NSMutableDictionary *out=[NSMutableDictionary dictionary];
 const uint32_t n=nmb_function_constant_count(_handle);
 for(uint32_t i=0;i<n;i++) {
  char name[256];uint64_t index=0,type=0;bool required=false;
  if(!nmb_function_constant_at(_handle,i,name,sizeof name,&index,&type,&required))continue;
  NBFunctionConstant *c=[NBFunctionConstant new];c.nbName=@(name);c.nbType=(MTLDataType)type;c.nbIndex=index==UINT64_MAX?NSNotFound:(NSUInteger)index;
  c.nbRequired=required;out[c.nbName]=c;
 }
 return out;
}
- (MTLFunctionOptions)options {return MTLFunctionOptionNone;}
@end
// The target's own accessors of MTLFunctionConstantValues (as the offline frontend reads them); their ABI is checked before any call.
@interface MTLFunctionConstantValues (NBOriginalConstantAccess)
- (NSArray *)newIndexedConstantArray;
- (NSArray *)newNamedConstantArray;
@end
@protocol NBOriginalConstantValue
- (NSUInteger)dataType;
- (void *)data;
- (NSUInteger)index;
- (NSString *)name;
@end
static bool methodABI(id object,SEL selector,const char *encoding) {
 Method method=class_getInstanceMethod(object_getClass(object),selector);
 return method && !strcmp(method_getTypeEncoding(method),encoding);
}
// Bytes of a function constant of MTLDataType `type` (scalars and 2/4-wide vectors; 3-wide vectors are not qualified).
static uint64_t constantBytes(uint64_t type) {
 switch(type) {
  case 3:case 29:case 33:return 4;
  case 4:case 30:case 34:return 8;
  case 6:case 32:case 36:return 16;
  case 16:case 37:case 41:return 2;
  case 17:case 38:case 42:return 4;
  case 19:case 40:case 44:return 8;
  case 45:case 49:case 53:return 1;
  case 46:case 50:case 54:return 2;
  case 48:case 52:case 56:return 4;
  case 81:case 85:return 8;
  case 82:case 86:return 16;
  case 84:case 88:return 32;
  // data-a-live: 3-wide vectors, lanes packed as metal2vulkan's canon/fc.rs reads them (the first
  // lanes*width bytes; Metal's storage is at least that). iOS 16 backboardd specializes a uchar3 (bb8).
  case 5:case 31:case 35:return 12;
  case 18:case 39:case 43:return 6;
  case 47:case 51:case 55:return 3;
  case 83:case 87:return 24;
  default:return 0;
 }
}
static std::string quartzCoreCallers();
@implementation NBLibrary
- (void)dealloc {nmb_library_release(_handle);}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values error:(NSError **)error {
 if(!name || !values || !methodABI(values,@selector(newIndexedConstantArray),"@16@0:8") || !methodABI(values,@selector(newNamedConstantArray),"@16@0:8")) {
  if(error)*error=nbError(@"unqualified function constant values");return nil;
 }
 MTLFunctionConstantValues *snapshot=[values copy];
 NSArray *indexed=[snapshot newIndexedConstantArray],*named=[snapshot newNamedConstantArray];
 std::vector<NmbConstant> constants;
 for(unsigned group=0;group<2;group++)for(id value in (group?named:indexed)) {
  if(!methodABI(value,@selector(dataType),"Q16@0:8") || !methodABI(value,@selector(data),"^v16@0:8") ||
     !methodABI(value,group?@selector(name):@selector(index),group?"@16@0:8":"Q16@0:8")) {
   if(error)*error=nbError(@"unqualified constant value ABI");return nil;
  }
  id<NBOriginalConstantValue> v=value;
  const uint64_t bytes=constantBytes(v.dataType);
  if(!bytes || !v.data){if(error)*error=nbError([NSString stringWithFormat:@"constant type %lu is not qualified",(unsigned long)v.dataType]);return nil;}
  constants.push_back({v.dataType,group?0:v.index,group?v.name.UTF8String:nullptr,v.data,bytes});
 }
 std::string text;
 for(const NmbConstant &c:constants) {
  char head[32];if(!c.name)snprintf(head,sizeof head,"#%llu",(unsigned long long)c.index);
  text+=text.empty()?"":" ";text+=c.name?c.name:head;snprintf(head,sizeof head,":%llu:",(unsigned long long)c.type);text+=head;
  for(uint64_t i=0;i<c.length;i++){snprintf(head,sizeof head,"%02x",((const uint8_t *)c.bytes)[i]);text+=head;}
 }
 const uint64_t functionBegan=nowUs();
 NmbFunction *handle=nmb_function_new_specialized(_handle,name.UTF8String,constants.data(),(uint32_t)constants.size());gTimeFunction.add(functionBegan);
 // One S line per specialization (int-155: every QuartzCore pipeline was made on its draw, and a refused generic function would leave its
 // generic pipeline unmade without a word): the function, its constants, the time, the refusal and QuartzCore's callers.
 if(!gJournalPath.empty()) {
  char head[96];snprintf(head,sizeof head,"S t=%llu us=%llu ok=%d name=",(unsigned long long)((functionBegan-gJournalBegan)/1000),
      (unsigned long long)(nowUs()-functionBegan),handle?1:0);
  journal(head+std::string(name.UTF8String?:"?")+" k="+text+(handle?std::string():" err="+std::string(lastError().UTF8String?:"?"))+" | "+quartzCoreCallers());
 }
 if(!handle){
  noteProblem([NSString stringWithFormat:@"specialized function %@: %@",name,lastError()]);
  if(error)*error=nbError([NSString stringWithFormat:@"specialized function: %@",lastError()]);return nil;
 }
 NBFunction *function=[NBFunction new];function.handle=handle;function.name=name;function.functionType=(MTLFunctionType)nmb_function_type(handle);
 function.constantsText=text;if(error)*error=nil;return (id<MTLFunction>)function;
}
- (void)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values completionHandler:(void (^)(id<MTLFunction>,NSError *))handler {
 NSError *error=nil;id<MTLFunction> function=[self newFunctionWithName:name constantValues:values error:&error];
 handler(function,error);
}
- (MTLLibraryType)type {return MTLLibraryTypeExecutable;}
// The descriptor's name and constants; its specialized name only labels Metal's function and its options ask for compile-time work the
// runtime does when the function is made.
- (id<MTLFunction>)newFunctionWithDescriptor:(MTLFunctionDescriptor *)d error:(NSError **)error {
 if(!d.name){if(error)*error=nbError(@"a function name is required");return nil;}
 if(d.constantValues)return [self newFunctionWithName:d.name constantValues:d.constantValues error:error];
 id<MTLFunction> function=[self newFunctionWithName:d.name];
 if(!function){if(error)*error=nbError([NSString stringWithFormat:@"function %@: %@",d.name,lastError()]);return nil;}
 if(error)*error=nil;return function;
}
// dada: answered by a neutral default before, whose handler never ran (a caller waiting on it waits for ever).
- (void)newFunctionWithDescriptor:(MTLFunctionDescriptor *)d completionHandler:(void (^)(id<MTLFunction>,NSError *))handler {
 NSError *error=nil;id<MTLFunction> function=[self newFunctionWithDescriptor:d error:&error];
 if(handler)handler(function,error);
}
- (NSArray<NSString *> *)functionNames {
 NSMutableArray *names=[NSMutableArray array];
 for(uint32_t i=0,n=nmb_library_function_count(_handle);i<n;i++) {
  char name[256];nmb_library_function_name(_handle,i,name,sizeof name);[names addObject:[NSString stringWithUTF8String:name]];
 }
 return names;
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name {
 const uint64_t functionBegan=nowUs();
 NmbFunction *handle=name?nmb_function_new(_handle,name.UTF8String):nullptr;gTimeFunction.add(functionBegan);
 if(!handle)return nil;
 NBFunction *function=[NBFunction new];function.handle=handle;function.name=name;function.functionType=(MTLFunctionType)nmb_function_type(handle);
 return (id<MTLFunction>)function;
}
@end
@interface NBComputePipeline : NBObject
@property(nonatomic) NmbComputePipeline *handle;
@end
@implementation NBComputePipeline
- (void)dealloc {nmb_compute_pipeline_release(_handle);}
- (NSUInteger)maxTotalThreadsPerThreadgroup {return nmb_compute_pipeline_max_threads(_handle);}
- (NSUInteger)threadExecutionWidth {return nmb_compute_pipeline_execution_width(_handle);}
@end
@interface NBRenderPipeline : NBObject
@property(nonatomic) NmbRenderPipeline *handle;
@end
@implementation NBRenderPipeline
- (void)dealloc {nmb_render_pipeline_release(_handle);}
// The threadgroup size a tile dispatch of this pipeline could use: the device's limit (no tile shaders are offered).
- (NSUInteger)maxTotalThreadsPerThreadgroup {
 uint64_t s[3]={};if(!nmb_device_max_threads_per_threadgroup(gDevice,s))return 0;
 return s[0];
}
@end
@interface NBSampler : NBObject
@property(nonatomic) NmbSampler *handle;
@end
@implementation NBSampler
- (void)dealloc {nmb_sampler_release(_handle);}
- (NSUInteger)pixelFormat {return MTLPixelFormatInvalid;}   // SPI: a sampler built from a plain descriptor names no format
@end
@interface NBDepthStencil : NBObject
@property(nonatomic) NmbDepthStencil *handle;
@end
@implementation NBDepthStencil
- (void)dealloc {nmb_depth_stencil_release(_handle);}
@end

// ---- events and fences ----
@interface NBEvent : NBObject
@property(nonatomic) NmbEvent *handle;
@end
@implementation NBEvent
- (void)dealloc {nmb_event_release(_handle);}
@end
@interface NBSharedEvent : NBEvent
@end
static void notified(void *context,uint64_t value);
@implementation NBSharedEvent
- (uint64_t)signaledValue {return nmb_shared_event_value(self.handle);}
- (void)setSignaledValue:(uint64_t)value {nmb_shared_event_set_value(self.handle,value);}
- (BOOL)waitUntilSignaledValue:(uint64_t)value timeoutMS:(uint64_t)milliseconds {return nmb_shared_event_wait(self.handle,value,milliseconds);}
// The block runs on the listener's dispatch queue (Metal's contract); the bridge's listener thread only forwards it there.
- (void)notifyListener:(MTLSharedEventListener *)listener atValue:(uint64_t)value block:(MTLSharedEventNotificationBlock)block {
 if(!block)return;
 dispatch_queue_t queue=listener.dispatchQueue?:dispatch_get_global_queue(QOS_CLASS_DEFAULT,0);
 NSArray *job=@[self,queue,[block copy]];
 if(!nmb_shared_event_notify(self.handle,value,notified,(__bridge_retained void *)job)) {
  (void)(__bridge_transfer NSArray *)(__bridge void *)job;
  NBLOG(@"native-metal: notifyListener refused: %@",lastError());
 }
}
// A handle for another process needs a kernel-backed event this bridge does not have yet: refused, never a handle that could not work.
- (MTLSharedEventHandle *)newSharedEventHandle {
 if(gMissing)gMissing("NBSharedEvent","newSharedEventHandle (cross-process)");
 return nil;
}
@end
static void notified(void *context,uint64_t value) {
 NSArray *job=(__bridge_transfer NSArray *)context;
 id<MTLSharedEvent> event=job[0];MTLSharedEventNotificationBlock block=job[2];
 dispatch_async(job[1],^{block(event,value);});
}
@interface NBFence : NBObject
@property(nonatomic) NmbFence *handle;
@end
@implementation NBFence
- (void)dealloc {nmb_fence_release(_handle);}
@end

// ---- command buffers and encoders ----
@interface NBQueue : NBObject
@property(nonatomic) NmbQueue *handle;
@property(atomic,strong) dispatch_queue_t completionQueue;   // SPI: where command buffers' handlers run (nil: the runtime's thread)
@property(nonatomic,strong) dispatch_semaphore_t slots;     // dada: Metal's maxCommandBufferCount: unfinished command buffers at most
@end
// dada: what a process draws into IOSurfaces (layers' and drawables' contents), on the console every 2 s: the last target's size, format
// and 9 pixels sampled over it, read back through the surface after the command buffer completes (g2k12: the lock screen's wallpaper is
// black, and whether its process draws black or nothing was not known).
static char gSurfaceProbeKey,gSurfaceSampledKey;
static void noteSampledSurface(id commandBuffer,NBTexture *texture) {
 if(!commandBuffer)return;
 NSMutableArray *list=objc_getAssociatedObject(commandBuffer,&gSurfaceSampledKey);
 if(!list){list=[NSMutableArray array];objc_setAssociatedObject(commandBuffer,&gSurfaceSampledKey,list,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}
 if(list.count<6 && ![list containsObject:texture])[list addObject:texture];
}
static NSString *surfaceMiddles(NSArray *textures) {
 NSMutableString *out=[NSMutableString string];
 for(NBTexture *t in textures) {
  IOSurfaceRef s=t.surface;uint32_t mid=0,quarter=0;
  if(s && IOSurfaceLock(s,kIOSurfaceLockReadOnly,nullptr)==0) {
   const uint8_t *b=(const uint8_t *)IOSurfaceGetBaseAddress(s);const size_t row=IOSurfaceGetBytesPerRow(s),bpe=IOSurfaceGetBytesPerElement(s);
   const size_t w=IOSurfaceGetWidth(s),h=IOSurfaceGetHeight(s);
   if(b && bpe==4){memcpy(&mid,b+(h/2)*row+(w/2)*4,4);memcpy(&quarter,b+(h*3/4)*row+(w/4)*4,4);}
   IOSurfaceUnlock(s,kIOSurfaceLockReadOnly,nullptr);
  }
  [out appendFormat:@" [%u pf %lu %08x %08x]",s?IOSurfaceGetID(s):0,(unsigned long)t.pixelFormat,mid,quarter];
 }
 return out;
}
static void surfaceProbe(NBTexture *target,bool failed,NSArray *sampled) {
 static std::atomic<uint64_t> last{0},frames{0},failures{0};
 frames++;if(failed)failures++;
 const uint64_t now=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);uint64_t then=last.load();
 if(now-then<2000000000ull || !last.compare_exchange_strong(then,now))return;
 IOSurfaceRef surface=target.surface;if(!surface)return;
 const size_t w=IOSurfaceGetWidth(surface),h=IOSurfaceGetHeight(surface),row=IOSurfaceGetBytesPerRow(surface),bpe=IOSurfaceGetBytesPerElement(surface);
 const OSType f=IOSurfaceGetPixelFormat(surface);
 NSMutableString *line=[NSMutableString stringWithFormat:@"surface probe: %llu frames (%llu failed) since; last target surface %u %zux%zu '%c%c%c%c' pf %lu:",
  (unsigned long long)frames.exchange(0),(unsigned long long)failures.exchange(0),IOSurfaceGetID(surface),w,h,(char)(f>>24),(char)(f>>16),(char)(f>>8),(char)f,
  (unsigned long)target.pixelFormat];
 if(IOSurfaceLock(surface,kIOSurfaceLockReadOnly,nullptr)==0) {
  const uint8_t *base=(const uint8_t *)IOSurfaceGetBaseAddress(surface);
  for(int k=0;k<9 && base && bpe && bpe<=8;k++) {
   const size_t x=w*(1+k%3*2)/6,y=h*(1+k/3*2)/6;uint64_t v=0;memcpy(&v,base+y*row+x*bpe,bpe);
   [line appendFormat:@" %0*llx",(int)(bpe*2),(unsigned long long)v];
  }
  IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,nullptr);
 } else [line appendString:@" (no lock)"];
 if(sampled.count)[line appendFormat:@"; sampled%@",surfaceMiddles(sampled)];
 consoleLine(line);
}
@interface NBCommandBuffer : NBObject {
@public
 EncodedWork encodedWork;   // the command buffer's single encoder thread, not global counter deltas
 uint64_t createdUs;
}
@property(nonatomic) NmbCommandBuffer *handle;
@property(nonatomic,strong) NBQueue *queue;
@property(atomic,copy) NSString *encodeFailure;
@property(atomic) BOOL committed,failedLocally;
@property(nonatomic) uint64_t committedUs;
@property(nonatomic) uint32_t commitIndex;
@property(nonatomic,strong) NSMutableArray *scheduledHandlers,*completedHandlers;
@property(nonatomic,strong) NSMutableSet *surfaces;
// Runtime handles retain GPU storage, but only the Objective-C owner retains
// a no-copy deallocator, its memory owner and texture/heap ancestry.
@property(nonatomic,strong) NSMutableSet *resourceOwners;
@property(nonatomic,strong) dispatch_group_t handlers;
@property(nonatomic,strong) dispatch_group_t schedulingHandlers;
@property(nonatomic) BOOL schedulingDelivered;
@property(atomic) BOOL holdsSlot;   // dada: one of its queue's slots, given back when it completes or goes
- (void)refuse:(NSString *)what;
@end
@interface NBEncoder : NBObject
@property(nonatomic) NmbEncoder *handle;
@property(nonatomic,strong) NBCommandBuffer *commandBuffer;
@end
static void waitForResource(id resource) {
 const uint64_t began=nowUs();
 NSArray *pending;@synchronized(gInFlight){pending=gInFlight.allObjects;}
 for(NBCommandBuffer *commandBuffer in pending) {
  int dependency=-1;
  if([resource isKindOfClass:[NBBuffer class]])dependency=nmb_command_buffer_uses_buffer(commandBuffer.handle,[(NBBuffer *)resource handle]);
  else if([resource isKindOfClass:[NBTexture class]])dependency=nmb_command_buffer_uses_texture(commandBuffer.handle,[(NBTexture *)resource handle]);
  gResourceChecks++;
  // An unknown result retains the old conservative wait. False means no matching storage
  // (or GPU work already completed), not a fabricated completion for a pending dependency.
  if(dependency!=0)[(id<MTLCommandBuffer>)commandBuffer waitUntilCompleted];else gResourceSkipped++;
 }
 gTimeResourceWait.add(began);
}
@implementation NBEncoder
- (void)dealloc {if(_handle)nmb_encoder_discard(_handle);}
- (bool)check:(bool)ok what:(const char *)what {
 if(!ok)[_commandBuffer refuse:[NSString stringWithFormat:@"%s: %@",what,lastError()]];
 return ok;
}
- (void)endEncoding {
 NmbEncoder *handle=_handle;_handle=nullptr;
 if(!handle){[_commandBuffer refuse:@"endEncoding twice"];return;}
 [self check:nmb_encoder_end(handle) what:"endEncoding"];
}
- (void)insertDebugSignpost:(NSString *)s {(void)s;}
- (void)pushDebugGroup:(NSString *)s {(void)s;}
- (void)popDebugGroup {}
// Stage 0 compute, 1 vertex, 2 fragment (the bridge's numbering).
- (void)buffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset index:(NSUInteger)index stage:(uint32_t)stage {
 if(buffer && ![(id)buffer isKindOfClass:[NBBuffer class]]){[_commandBuffer refuse:@"foreign buffer"];return;}
 if(buffer)[_commandBuffer.resourceOwners addObject:buffer];
 if(pandoraAstcProbe())   // data-a-live: the buffer itself, for the probe's CPU/GPU comparison
  probeBindings(self)[[NSString stringWithFormat:@"~%sbuffer%lu",probeStage(stage),(unsigned long)index]]=buffer?@[buffer,@(offset)]:@[];
 if(pandoraAstcProbe()) {   // data-a-live: remembered for the probe's draw log, with the first bytes from the offset
  const uint8_t *contents=buffer?(const uint8_t *)buffer.contents:nullptr;
  probeBindings(self)[[NSString stringWithFormat:@"%sbuffer%lu",probeStage(stage),(unsigned long)index]]=!buffer?@"none":
   [NSString stringWithFormat:@"len %lu off %lu %@",(unsigned long)buffer.length,(unsigned long)offset,
    contents && offset<buffer.length?probeHex(contents+offset,buffer.length-offset,768):@"(no contents)"];
 }
 if(capturing() && buffer) {   // dada (nbcap): the bound bytes from the offset (the wallpaper's uniforms, g2k61)
  const uint8_t *c=(const uint8_t *)buffer.contents;
  if(c && offset<buffer.length)capture([NSString stringWithFormat:@"buf %s%lu len %lu off %lu %@",stage==1?"v":stage==2?"f":"c",(unsigned long)index,
                                         (unsigned long)buffer.length,(unsigned long)offset,probeHex(c+offset,buffer.length-offset,256)]);
 }
 [self check:nmb_encoder_set_buffer(_handle,stage,buffer?[(NBBuffer *)buffer handle]:nullptr,offset,(uint32_t)index) what:"setBuffer"];
}
- (void)texture:(id<MTLTexture>)texture index:(NSUInteger)index stage:(uint32_t)stage {
 if([(id)texture isKindOfClass:[NBTexture class]] && [(NBTexture *)texture surface] && texture.width*texture.height>=1000000)
  noteSampledSurface(self.commandBuffer,(NBTexture *)texture);   // dada: the surface probe's screen-sized sources
 if(capturing())capture([NSString stringWithFormat:@"tex %s%lu %@",stage==1?"v":stage==2?"f":"c",(unsigned long)index,capTexture(texture)]);
 // dada (nbcap): a small RGBA16Float texture's values, once each -- the lock screen's wallpaper samples one for its background, and
 // whether that is HDR (above 1) or wrong data decides where its overexposure comes from (g2k60)
 if(capturing() && texture && texture.pixelFormat==MTLPixelFormatRGBA16Float && texture.width*texture.height<=262144) {
  static NSMutableSet *seen;if(!seen)seen=[NSMutableSet set];
  if(![seen containsObject:capId(texture)]) {
   [seen addObject:capId(texture)];
   const NSUInteger w=texture.width,h=texture.height;std::vector<__fp16> px(w*h*4);
   [texture getBytes:px.data() bytesPerRow:w*8 fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
   float lo[4]={1e9,1e9,1e9,1e9},hi[4]={-1e9,-1e9,-1e9,-1e9};
   for(size_t i=0;i<px.size();i++){const float v=(float)px[i];lo[i&3]=std::min(lo[i&3],v);hi[i&3]=std::max(hi[i&3],v);}
   capture([NSString stringWithFormat:@"texvals %@ r %.3f..%.3f g %.3f..%.3f b %.3f..%.3f a %.3f..%.3f; first %.3f %.3f %.3f %.3f, middle %.3f %.3f %.3f %.3f",
            capId(texture),lo[0],hi[0],lo[1],hi[1],lo[2],hi[2],lo[3],hi[3],(float)px[0],(float)px[1],(float)px[2],(float)px[3],
            (float)px[(h/2*w+w/2)*4],(float)px[(h/2*w+w/2)*4+1],(float)px[(h/2*w+w/2)*4+2],(float)px[(h/2*w+w/2)*4+3]]);
  }
 }
 // dada (g2k64): a tiny RGBA8 texture whole (the wallpaper's 64x8 gradients), and the middle row of the RGBA16Float env map, for a Mac replay
 if(capturing() && texture && texture.pixelFormat==MTLPixelFormatRGBA8Unorm && texture.width*texture.height<=1024 && !texture.framebufferOnly) {
  static NSMutableSet *seen8;if(!seen8)seen8=[NSMutableSet set];
  if(![seen8 containsObject:capId(texture)]) {
   [seen8 addObject:capId(texture)];
   const NSUInteger w=texture.width,h=texture.height;std::vector<uint8_t> px(w*h*4);
   [texture getBytes:px.data() bytesPerRow:w*4 fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
   for(NSUInteger y=0;y<h;y++)capture([NSString stringWithFormat:@"texrow %@ y%lu %@",capId(texture),(unsigned long)y,probeHex(px.data()+y*w*4,w*4,w*4)]);
  }
 }
 if(capturing() && texture && texture.pixelFormat==MTLPixelFormatRGBA16Float && texture.width==512 && texture.height==256) {
  static NSMutableSet *seenh;if(!seenh)seenh=[NSMutableSet set];
  if(![seenh containsObject:capId(texture)]) {
   [seenh addObject:capId(texture)];
   std::vector<__fp16> px(512*4);NSMutableString *o=[NSMutableString string];
   [texture getBytes:px.data() bytesPerRow:512*8 fromRegion:MTLRegionMake2D(0,128,512,1) mipmapLevel:0];
   for(int x=0;x<512;x+=16)[o appendFormat:@" %d:%.3f,%.3f,%.3f",x,(float)px[x*4],(float)px[x*4+1],(float)px[x*4+2]];
   [o appendFormat:@" 511:%.3f,%.3f,%.3f",(float)px[511*4],(float)px[511*4+1],(float)px[511*4+2]];
   capture([NSString stringWithFormat:@"envrow %@%@",capId(texture),o]);
  }
 }
 if(pandoraAstcProbe())probeBindings(self)[[NSString stringWithFormat:@"%stex%lu",probeStage(stage),(unsigned long)index]]=capTexture(texture);
 if(texture && ![(id)texture isKindOfClass:[NBTexture class]]){[_commandBuffer refuse:@"foreign texture"];return;}
 if(texture)[_commandBuffer.resourceOwners addObject:texture];
 if(texture && [(NBTexture *)texture surface])[_commandBuffer.surfaces addObject:texture];
 if(texture && pandoraAstcProbe() && ((texture.pixelFormat==MTLPixelFormatASTC_4x4_LDR && texture.width>=512) ||
                                       objc_getAssociatedObject(texture,&gProbeReadbackKey))) {
  id target=objc_getAssociatedObject(self,&gProbePassTargetKey);   // data-a-live: read it back at completion
  if(target) {
   NSMutableSet *targets=objc_getAssociatedObject(_commandBuffer,&gProbeReadbackKey);
   if(!targets){targets=[NSMutableSet set];objc_setAssociatedObject(_commandBuffer,&gProbeReadbackKey,targets,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}
   [targets addObject:target];
  }
  probeBindings(self)[@"armed"]=[NSString stringWithFormat:@"%lux%lu f%lu at %s%lu",(unsigned long)texture.width,
   (unsigned long)texture.height,(unsigned long)texture.pixelFormat,probeStage(stage),(unsigned long)index];
 }
 [self check:nmb_encoder_set_texture(_handle,stage,texture?[(NBTexture *)texture handle]:nullptr,(uint32_t)index) what:"setTexture"];
}
- (void)sampler:(id<MTLSamplerState>)sampler index:(NSUInteger)index stage:(uint32_t)stage {
 if(sampler && ![(id)sampler isKindOfClass:[NBSampler class]]){[_commandBuffer refuse:@"foreign sampler"];return;}
 if(pandoraAstcProbe()) {   // data-a-live: remembered for the probe's draw log
  NSString *what=sampler?objc_getAssociatedObject(sampler,&gProbeSamplerKey):nil;
  probeBindings(self)[[NSString stringWithFormat:@"%ssampler%lu",probeStage(stage),(unsigned long)index]]=what?:(sampler?@"?":@"none");
 }
 [self check:nmb_encoder_set_sampler(_handle,stage,sampler?[(NBSampler *)sampler handle]:nullptr,(uint32_t)index) what:"setSamplerState"];
}
- (void)bytes:(const void *)bytes length:(NSUInteger)length index:(NSUInteger)index stage:(uint32_t)stage {
 if(pandoraAstcProbe() && bytes)   // data-a-live: remembered for the probe's draw log
  probeBindings(self)[[NSString stringWithFormat:@"%sbytes%lu",probeStage(stage),(unsigned long)index]]=probeHex(bytes,length,768);
 if(capturing() && bytes)capture([NSString stringWithFormat:@"bytes %s%lu len %lu %@",stage==1?"v":stage==2?"f":"c",(unsigned long)index,
                                         (unsigned long)length,probeHex(bytes,length,256)]);
 if(capturing() && bytes && length>256)   // dada (g2k63): the rest, Mercury's 432..485 (intensity, isBackgroundPass)
  capture([NSString stringWithFormat:@"bytes+256 %s%lu %@",stage==1?"v":stage==2?"f":"c",(unsigned long)index,
           probeHex((const uint8_t *)bytes+256,length-256,256)]);
 [self check:nmb_encoder_set_bytes(_handle,stage,bytes,length,(uint32_t)index) what:"setBytes"];
}
- (void)fence:(id<MTLFence>)fence stages:(uint64_t)stages update:(bool)update {
 if(![(id)fence isKindOfClass:[NBFence class]]){[_commandBuffer refuse:@"foreign fence"];return;}
 NmbFence *handle=[(NBFence *)fence handle];
 [self check:update?nmb_encoder_update_fence(_handle,handle,stages):nmb_encoder_wait_fence(_handle,handle,stages) what:update?"updateFence":"waitForFence"];
}
- (void)updateFence:(id<MTLFence>)fence {[self fence:fence stages:0 update:true];}
- (void)waitForFence:(id<MTLFence>)fence {[self fence:fence stages:0 update:false];}
- (void)offset:(NSUInteger)offset index:(NSUInteger)index stage:(uint32_t)stage {
 if(pandoraAstcProbe()) {   // data-a-live: the binding moves; the probe's draw log and watch follow it
  NSMutableDictionary *seen=probeBindings(self);
  NSString *key=[NSString stringWithFormat:@"~%sbuffer%lu",probeStage(stage),(unsigned long)index];
  NSArray *bound=seen[key];
  if(bound.count==2)seen[key]=@[bound[0],@(offset)];
  seen[[NSString stringWithFormat:@"%sbufferoffset%lu",probeStage(stage),(unsigned long)index]]=@(offset);
 }
 [self check:nmb_encoder_set_buffer_offset(_handle,stage,offset,(uint32_t)index) what:"setBufferOffset"];
}
@end

@interface NBComputeEncoder : NBEncoder
@end
@implementation NBComputeEncoder
// Metal's per-object trace identity (iOS/macOS 26 QuartzCore asks an encoder for it): one number per encoder, never reused.
- (uint64_t)globalTraceObjectID {static std::atomic<uint64_t> next{1};static char key;
 NSNumber *id_=objc_getAssociatedObject(self,&key);if(!id_){id_=@(next++);objc_setAssociatedObject(self,&key,id_,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}
 return id_.unsignedLongLongValue;}
- (MTLDispatchType)dispatchType {return MTLDispatchTypeSerial;}
- (void)setComputePipelineState:(id<MTLComputePipelineState>)state {
 if(![(id)state isKindOfClass:[NBComputePipeline class]]){[self.commandBuffer refuse:@"foreign compute pipeline"];return;}
 [self check:nmb_compute_set_pipeline(self.handle,[(NBComputePipeline *)state handle]) what:"setComputePipelineState"];
}
- (void)setBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i {[self buffer:b offset:o index:i stage:0];}
- (void)setBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i {[self offset:o index:i stage:0];}
- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained [])b offsets:(const NSUInteger [])o withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++)[self buffer:b[k] offset:o[k] index:r.location+k stage:0];
}
- (void)setBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {[self bytes:p length:n index:i stage:0];}
- (void)setTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {[self texture:t index:i stage:0];}
- (void)setTextures:(const id<MTLTexture> __unsafe_unretained [])t withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++)[self texture:t[k] index:r.location+k stage:0];
}
- (void)setSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i {[self sampler:s index:i stage:0];}
- (void)setThreadgroupMemoryLength:(NSUInteger)n atIndex:(NSUInteger)i {
 [self check:nmb_compute_set_threadgroup_memory(self.handle,(uint32_t)n,(uint32_t)i) what:"setThreadgroupMemoryLength"];
}
- (void)dispatch:(MTLSize)size group:(MTLSize)group threads:(bool)threads {
 gDispatches++;self.commandBuffer->encodedWork.dispatches++;
 const uint64_t s[3]={size.width,size.height,size.depth},g[3]={group.width,group.height,group.depth};
 [self check:nmb_compute_dispatch(self.handle,s,g,threads) what:"dispatch"];
}
- (void)dispatchThreads:(MTLSize)t threadsPerThreadgroup:(MTLSize)g {[self dispatch:t group:g threads:true];}
- (void)dispatchThreadgroups:(MTLSize)t threadsPerThreadgroup:(MTLSize)g {[self dispatch:t group:g threads:false];}
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u {(void)r;(void)u;}
- (void)memoryBarrierWithScope:(MTLBarrierScope)s {(void)s;}
@end

@interface NBRenderEncoder : NBEncoder
@end
@implementation NBRenderEncoder
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state {
 if(capturing())capture([NSString stringWithFormat:@"pipe %@ %@",capId(state),objc_getAssociatedObject(state,&gCapNamesKey)]);
 if(pandoraAstcProbe())probeBindings(self)[@"pipe"]=objc_getAssociatedObject(state,&gProbePipeKey)?:@"?";
 if(pandoraAstcProbe())probeBindings(self)[@"pipeid"]=capId(state);
 if(![(id)state isKindOfClass:[NBRenderPipeline class]]){[self.commandBuffer refuse:@"foreign render pipeline"];return;}
 [self check:nmb_render_set_pipeline(self.handle,[(NBRenderPipeline *)state handle]) what:"setRenderPipelineState"];
}
- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i {[self buffer:b offset:o index:i stage:1];}
- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i {[self buffer:b offset:o index:i stage:2];}
- (void)setVertexBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i {[self offset:o index:i stage:1];}
- (void)setFragmentBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i {[self offset:o index:i stage:2];}
- (void)setVertexBuffers:(const id<MTLBuffer> __unsafe_unretained [])b offsets:(const NSUInteger [])o withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++)[self buffer:b[k] offset:o[k] index:r.location+k stage:1];
}
- (void)setFragmentBuffers:(const id<MTLBuffer> __unsafe_unretained [])b offsets:(const NSUInteger [])o withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++)[self buffer:b[k] offset:o[k] index:r.location+k stage:2];
}
- (void)setVertexBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {[self bytes:p length:n index:i stage:1];}
- (void)setFragmentBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {[self bytes:p length:n index:i stage:2];}
- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {[self texture:t index:i stage:1];}
- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {[self texture:t index:i stage:2];}
- (void)setFragmentTextures:(const id<MTLTexture> __unsafe_unretained [])t withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++)[self texture:t[k] index:r.location+k stage:2];
}
- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i {[self sampler:s index:i stage:1];}
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i {[self sampler:s index:i stage:2];}
- (void)setDepthStencilState:(id<MTLDepthStencilState>)state {
 if(state && ![(id)state isKindOfClass:[NBDepthStencil class]]){[self.commandBuffer refuse:@"foreign depth/stencil state"];return;}
 if(pandoraAstcProbe())probeBindings(self)[@"depthstencil"]=state?(objc_getAssociatedObject(state,&gProbeDepthKey)?:@"?"):@"none";
 [self check:nmb_render_set_depth_stencil(self.handle,state?[(NBDepthStencil *)state handle]:nullptr) what:"setDepthStencilState"];
}
- (void)setViewport:(MTLViewport)v {
 if(capturing())capture([NSString stringWithFormat:@"viewport %.1f %.1f %.1fx%.1f",v.originX,v.originY,v.width,v.height]);
 if(pandoraAstcProbe())probeBindings(self)[@"viewport"]=[NSString stringWithFormat:@"%g %g %gx%g z %g-%g",v.originX,v.originY,v.width,v.height,v.znear,v.zfar];
 const double values[6]={v.originX,v.originY,v.width,v.height,v.znear,v.zfar};
 [self check:nmb_render_set_viewport(self.handle,values) what:"setViewport"];
}
- (void)setScissorRect:(MTLScissorRect)r {
 if(capturing())capture([NSString stringWithFormat:@"scissor %lu %lu %lux%lu",(unsigned long)r.x,(unsigned long)r.y,(unsigned long)r.width,(unsigned long)r.height]);
 if(pandoraAstcProbe())probeBindings(self)[@"scissor"]=[NSString stringWithFormat:@"%lu %lu %lux%lu",(unsigned long)r.x,(unsigned long)r.y,(unsigned long)r.width,(unsigned long)r.height];
 const uint64_t values[4]={r.x,r.y,r.width,r.height};
 [self check:nmb_render_set_scissor(self.handle,values) what:"setScissorRect"];
}
// dada: answered by neutral defaults (nothing set) before. One viewport and one scissor rectangle are what the runtime takes: the first
// of each (a function that picks another by its viewport index draws through the first).
- (void)setViewports:(const MTLViewport *)viewports count:(NSUInteger)count {if(viewports && count)[self setViewport:viewports[0]];}
- (void)setScissorRects:(const MTLScissorRect *)rects count:(NSUInteger)count {if(rects && count)[self setScissorRect:rects[0]];}
// The runtime takes cull mode, winding and fill mode together; this encoder keeps Metal's defaults until each is set.
- (void)raster {
 NSNumber *cull=objc_getAssociatedObject(self,"cull"),*winding=objc_getAssociatedObject(self,"winding"),*fill=objc_getAssociatedObject(self,"fill");
 if(pandoraAstcProbe())probeBindings(self)[@"raster"]=[NSString stringWithFormat:@"cull %llu winding %llu fill %llu",
  cull.unsignedLongLongValue,winding.unsignedLongLongValue,fill.unsignedLongLongValue];
 [self check:nmb_render_set_raster(self.handle,cull.unsignedLongLongValue,winding.unsignedLongLongValue,fill.unsignedLongLongValue) what:"raster state"];
}
- (void)setCullMode:(MTLCullMode)m {objc_setAssociatedObject(self,"cull",@(m),OBJC_ASSOCIATION_RETAIN);[self raster];}
- (void)setFrontFacingWinding:(MTLWinding)w {objc_setAssociatedObject(self,"winding",@(w),OBJC_ASSOCIATION_RETAIN);[self raster];}
- (void)setTriangleFillMode:(MTLTriangleFillMode)f {objc_setAssociatedObject(self,"fill",@(f),OBJC_ASSOCIATION_RETAIN);[self raster];}
- (void)setDepthBias:(float)bias slopeScale:(float)slope clamp:(float)clamp {
 [self check:nmb_render_set_depth_bias(self.handle,bias,slope,clamp) what:"setDepthBias"];
}
- (void)setLineWidth:(float)width {[self check:nmb_render_set_line_width(self.handle,width) what:"setLineWidth"];}
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode offset:(NSUInteger)offset {
 [self check:nmb_render_set_visibility_mode(self.handle,mode,offset) what:"setVisibilityResultMode"];
}
- (void)setFragmentTexture:(id<MTLTexture>)t atTextureIndex:(NSUInteger)ti samplerState:(id<MTLSamplerState>)s atSamplerIndex:(NSUInteger)si {
 [self texture:t index:ti stage:2];[self sampler:s index:si stage:2];
}
// Tile shaders and imageblock tile memory (Apple GPU families 4 and up) are not offered: supportsTileShaders answers no, and a call that
// reaches one anyway fails its command buffer.
- (NSUInteger)tileWidth {return 0;}
- (NSUInteger)tileHeight {return 0;}
- (void)setTileTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {(void)t;(void)i;[self.commandBuffer refuse:@"tile shaders are not supported"];}
- (void)setTileBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {(void)p;(void)n;(void)i;[self.commandBuffer refuse:@"tile shaders are not supported"];}
- (void)dispatchThreadsPerTile:(MTLSize)size {(void)size;[self.commandBuffer refuse:@"tile shaders are not supported"];}
- (void)dispatchThreadsPerTile:(MTLSize)size inRegion:(MTLRegion)region {(void)size;(void)region;[self.commandBuffer refuse:@"tile shaders are not supported"];}
- (void)setStencilReferenceValue:(uint32_t)v {[self check:nmb_render_set_stencil_reference(self.handle,v,v) what:"setStencilReferenceValue"];}
- (void)setStencilFrontReferenceValue:(uint32_t)f backReferenceValue:(uint32_t)b {
 [self check:nmb_render_set_stencil_reference(self.handle,f,b) what:"setStencilReferenceValues"];
}
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a {
 const float c[4]={r,g,b,a};[self check:nmb_render_set_blend_color(self.handle,c) what:"setBlendColor"];
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n instanceCount:(NSUInteger)i baseInstance:(NSUInteger)b {
 if(capturing())capture([NSString stringWithFormat:@"draw p%lu v%lu+%lu i%lu",(unsigned long)t,(unsigned long)s,(unsigned long)n,(unsigned long)i]);
 if(pandoraAstcProbe() && objc_getAssociatedObject(self,&gProbeVisKey)) {   // data-a-live: count this draw's samples
  NSMutableArray *labels=objc_getAssociatedObject(self,&gProbeVisLabelsKey);
  if(labels && labels.count<64) {
   [labels addObject:[NSString stringWithFormat:@"%@pipe %@",probeBindings(self)[@"armed"]?@"PROBED ":@"",probeBindings(self)[@"pipeid"]?:@"?"]];
   [(id<MTLRenderCommandEncoder>)self setVisibilityResultMode:MTLVisibilityResultModeCounting offset:(labels.count-1)*8];
  }
 }
 if(pandoraAstcProbe() && (probeBindings(self)[@"armed"] || probeBindings(self)[@"following"])) {   // data-a-live: the probed draw's whole state, and the draws after it
  static std::atomic<int> told{0};
  const int nth=told.fetch_add(1);
  if(nth<48) {
   NSLog(@"pandora-metal replay d%d: draw prim %lu start %lu count %lu instances %lu",nth,(unsigned long)t,(unsigned long)s,(unsigned long)n,(unsigned long)i);
   NSDictionary *seen=probeBindings(self);
   for(NSString *key in [seen.allKeys sortedArrayUsingSelector:@selector(compare:)])probeLog([NSString stringWithFormat:@"d%d",nth],key,seen[key]);
  }
  [probeBindings(self) removeObjectForKey:@"armed"];probeBindings(self)[@"following"]=@YES;
 }
 gDraws++;self.commandBuffer->encodedWork.draws++;
 [self check:nmb_render_draw(self.handle,t,(uint32_t)s,(uint32_t)n,(uint32_t)i,(uint32_t)b) what:"drawPrimitives"];
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n instanceCount:(NSUInteger)i {
 [self drawPrimitives:t vertexStart:s vertexCount:n instanceCount:i baseInstance:0];
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n {
 [self drawPrimitives:t vertexStart:s vertexCount:n instanceCount:1 baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)k indexBuffer:(id<MTLBuffer>)b indexBufferOffset:(NSUInteger)o
                instanceCount:(NSUInteger)i baseVertex:(NSInteger)v baseInstance:(NSUInteger)bi {
 if(capturing())capture([NSString stringWithFormat:@"drawidx p%lu n%lu i%lu",(unsigned long)t,(unsigned long)n,(unsigned long)i]);
 if(pandoraAstcProbe() && objc_getAssociatedObject(self,&gProbeVisKey)) {   // data-a-live: count this draw's samples
  NSMutableArray *labels=objc_getAssociatedObject(self,&gProbeVisLabelsKey);
  if(probeBindings(self)[@"armed"] && !labels) {
   labels=[NSMutableArray array];objc_setAssociatedObject(self,&gProbeVisLabelsKey,labels,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
   NSMutableArray *reads=objc_getAssociatedObject(self.commandBuffer,&gProbeVisKey);
   if(!reads){reads=[NSMutableArray array];objc_setAssociatedObject(self.commandBuffer,&gProbeVisKey,reads,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}
   [reads addObject:@[objc_getAssociatedObject(self,&gProbeVisKey),labels]];
  }
  if(labels && labels.count<64) {
   [labels addObject:[NSString stringWithFormat:@"%@pipe %@",probeBindings(self)[@"armed"]?@"PROBED ":@"",probeBindings(self)[@"pipeid"]?:@"?"]];
   [(id<MTLRenderCommandEncoder>)self setVisibilityResultMode:MTLVisibilityResultModeCounting offset:(labels.count-1)*8];
  }
 }
 if(pandoraAstcProbe() && (probeBindings(self)[@"armed"] || probeBindings(self)[@"following"])) {   // data-a-live: the probed draw's whole state, and the draws after it
  static std::atomic<int> told{0};
  const int nth=told.fetch_add(1);
  if(nth<48) {
   const uint8_t *ic=b?(const uint8_t *)b.contents:nullptr;const NSUInteger isz=k==MTLIndexTypeUInt16?2:4;
   NSLog(@"pandora-metal replay %d: drawIndexed prim %lu count %lu type %lu offset %lu instances %lu baseVertex %ld indices %@",nth,
    (unsigned long)t,(unsigned long)n,(unsigned long)k,(unsigned long)o,(unsigned long)i,(long)v,
    ic && o<b.length?probeHex(ic+o,MIN(n*isz,b.length-o),96):@"(no contents)");
   NSDictionary *seen=probeBindings(self);
   for(NSString *key in [seen.allKeys sortedArrayUsingSelector:@selector(compare:)])
    if(![key hasPrefix:@"~"])probeLog([NSString stringWithFormat:@"%d",nth],key,seen[key]);
   // the buffers as the CPU holds them now; watched again at commit and after completion (the GPU's view too)
   NSMutableArray *watch=objc_getAssociatedObject(self.commandBuffer,&gProbeWatchKey);
   if(!watch){watch=[NSMutableArray array];objc_setAssociatedObject(self.commandBuffer,&gProbeWatchKey,watch,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}
   if(b)[watch addObject:@[b,@(o),[NSString stringWithFormat:@"%d index+%lu",nth,(unsigned long)o]]];
   for(NSString *key in seen)if([key hasPrefix:@"~"] && [seen[key] count]==2) {
    id<MTLBuffer> buffer=seen[key][0];const NSUInteger offset=[seen[key][1] unsignedLongValue];
    const uint8_t *c=(const uint8_t *)buffer.contents;
    NSString *label=[NSString stringWithFormat:@"%d %@+%lu",nth,[key substringFromIndex:1],(unsigned long)offset];
    NSLog(@"pandora-metal watch %@ at draw: %@ | %@",label,probeBufferKind(buffer),c && offset<buffer.length?probeHex(c+offset,buffer.length-offset,64):@"(no contents)");
    [watch addObject:@[buffer,@(offset),label]];
   }
  }
  [probeBindings(self) removeObjectForKey:@"armed"];probeBindings(self)[@"following"]=@YES;
 }
 if(![(id)b isKindOfClass:[NBBuffer class]]){[self.commandBuffer refuse:@"foreign index buffer"];return;}
 [self.commandBuffer.resourceOwners addObject:b];
 gDraws++;self.commandBuffer->encodedWork.draws++;
 [self check:nmb_render_draw_indexed(self.handle,t,(uint32_t)n,k,[(NBBuffer *)b handle],o,(uint32_t)i,(int32_t)v,(uint32_t)bi) what:"drawIndexedPrimitives"];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)k indexBuffer:(id<MTLBuffer>)b indexBufferOffset:(NSUInteger)o
                instanceCount:(NSUInteger)i {
 [self drawIndexedPrimitives:t indexCount:n indexType:k indexBuffer:b indexBufferOffset:o instanceCount:i baseVertex:0 baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)k indexBuffer:(id<MTLBuffer>)b indexBufferOffset:(NSUInteger)o {
 [self drawIndexedPrimitives:t indexCount:n indexType:k indexBuffer:b indexBufferOffset:o instanceCount:1 baseVertex:0 baseInstance:0];
}
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u stages:(MTLRenderStages)s {(void)r;(void)u;(void)s;}
- (void)updateFence:(id<MTLFence>)fence afterStages:(MTLRenderStages)stages {[self fence:fence stages:stages update:true];}
- (void)waitForFence:(id<MTLFence>)fence beforeStages:(MTLRenderStages)stages {[self fence:fence stages:stages update:false];}
@end

@interface NBBlitEncoder : NBEncoder
@end
@implementation NBBlitEncoder
// The arguments are passed as a plain array: an NSArray literal raises on a nil element, and a nil resource (a refused allocation) must fail the
// command buffer like any invalid argument, not abort the host process.
#define NB_OBJECTS(...) ((__unsafe_unretained id[]){__VA_ARGS__})
- (bool)buffers:(__unsafe_unretained id *)objects count:(size_t)count {
 for(size_t i=0;i<count;i++)if(![objects[i] isKindOfClass:[NBBuffer class]]){[self.commandBuffer refuse:@"missing or foreign blit buffer"];return false;}
 for(size_t i=0;i<count;i++)[self.commandBuffer.resourceOwners addObject:objects[i]];
 return true;
}
- (bool)textures:(__unsafe_unretained id *)objects count:(size_t)count {
 for(size_t i=0;i<count;i++) {
  id o=objects[i];
  if(![o isKindOfClass:[NBTexture class]]){[self.commandBuffer refuse:@"missing or foreign blit texture"];return false;}
  [self.commandBuffer.resourceOwners addObject:o];
  if([(NBTexture *)o surface])[self.commandBuffer.surfaces addObject:o];
 }
 return true;
}
- (void)copyFromBuffer:(id<MTLBuffer>)s sourceOffset:(NSUInteger)so toBuffer:(id<MTLBuffer>)d destinationOffset:(NSUInteger)dO size:(NSUInteger)n {
 if(![self buffers:NB_OBJECTS(s,d) count:2])return;
 [self check:nmb_blit_copy_buffer(self.handle,[(NBBuffer *)s handle],so,[(NBBuffer *)d handle],dO,n) what:"copyFromBuffer:toBuffer:"];
}
- (void)fillBuffer:(id<MTLBuffer>)b range:(NSRange)r value:(uint8_t)v {
 if(![self buffers:NB_OBJECTS(b) count:1])return;
 [self check:nmb_blit_fill_buffer(self.handle,[(NBBuffer *)b handle],r.location,r.location+r.length,v) what:"fillBuffer"];
}
- (void)copyFromTexture:(id<MTLTexture>)s sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)z
              toTexture:(id<MTLTexture>)d destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dO {
 if([(id)d isKindOfClass:[NBTexture class]] && [(NBTexture *)d surface])   // dada: the surface probe's target (a picture copied out)
  objc_setAssociatedObject(self.commandBuffer,&gSurfaceProbeKey,d,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
 if(capturing())capture([NSString stringWithFormat:@"copy %@ lv%lu %lu,%lu %lux%lu -> %@ lv%lu %lu,%lu",capTexture(s),(unsigned long)sl,(unsigned long)so.x,
  (unsigned long)so.y,(unsigned long)z.width,(unsigned long)z.height,capTexture(d),(unsigned long)dl,(unsigned long)dO.x,(unsigned long)dO.y]);
 if(![self textures:NB_OBJECTS(s,d) count:2])return;
 const uint64_t a[3]={so.x,so.y,so.z},size[3]={z.width,z.height,z.depth},b[3]={dO.x,dO.y,dO.z};
 [self check:nmb_blit_copy_texture(self.handle,[(NBTexture *)s handle],ss,sl,a,size,[(NBTexture *)d handle],ds,dl,b) what:"copyFromTexture:toTexture:"];
}
- (void)copyFromTexture:(id<MTLTexture>)s toTexture:(id<MTLTexture>)d {
 if(![self textures:NB_OBJECTS(s,d) count:2])return;
 const NSUInteger levels=MIN(s.mipmapLevelCount,d.mipmapLevelCount),slices=MIN(s.arrayLength,d.arrayLength);
 for(NSUInteger slice=0;slice<slices;slice++)for(NSUInteger level=0;level<levels;level++) {
  MTLSize size=MTLSizeMake(MAX(s.width>>level,(NSUInteger)1),MAX(s.height>>level,(NSUInteger)1),MAX(s.depth>>level,(NSUInteger)1));
  [self copyFromTexture:s sourceSlice:slice sourceLevel:level sourceOrigin:MTLOriginMake(0,0,0) sourceSize:size toTexture:d destinationSlice:slice
       destinationLevel:level destinationOrigin:MTLOriginMake(0,0,0)];
 }
}
// dada: answered by a neutral default (nothing copied) before: MercuryPoster, the lock screen's wallpaper, copies its picture out with it
// and the wallpaper stayed black (g2k10).
- (void)copyFromTexture:(id<MTLTexture>)s sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl toTexture:(id<MTLTexture>)d
       destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl sliceCount:(NSUInteger)slices levelCount:(NSUInteger)levels {
 if(![self textures:NB_OBJECTS(s,d) count:2])return;
 for(NSUInteger slice=0;slice<slices;slice++)for(NSUInteger level=0;level<levels;level++) {
  const NSUInteger l=sl+level;
  MTLSize size=MTLSizeMake(MAX(s.width>>l,(NSUInteger)1),MAX(s.height>>l,(NSUInteger)1),MAX(s.depth>>l,(NSUInteger)1));
  [self copyFromTexture:s sourceSlice:ss+slice sourceLevel:l sourceOrigin:MTLOriginMake(0,0,0) sourceSize:size toTexture:d
       destinationSlice:ds+slice destinationLevel:dl+level destinationOrigin:MTLOriginMake(0,0,0)];
 }
}
- (void)copyFromBuffer:(id<MTLBuffer>)s sourceOffset:(NSUInteger)o sourceBytesPerRow:(NSUInteger)row sourceBytesPerImage:(NSUInteger)image
            sourceSize:(MTLSize)z toTexture:(id<MTLTexture>)d destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level
     destinationOrigin:(MTLOrigin)dO {
 if(![self buffers:NB_OBJECTS(s) count:1] || ![self textures:NB_OBJECTS(d) count:1])return;
 const uint64_t size[3]={z.width,z.height,z.depth},b[3]={dO.x,dO.y,dO.z};
 [self check:nmb_blit_buffer_to_texture(self.handle,[(NBBuffer *)s handle],o,row,image,size,[(NBTexture *)d handle],slice,level,b) what:"copyFromBuffer:toTexture:"];
}
- (void)copyFromTexture:(id<MTLTexture>)s sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)z
               toBuffer:(id<MTLBuffer>)d destinationOffset:(NSUInteger)o destinationBytesPerRow:(NSUInteger)row destinationBytesPerImage:(NSUInteger)image {
 if(![self textures:NB_OBJECTS(s) count:1] || ![self buffers:NB_OBJECTS(d) count:1])return;
 const uint64_t a[3]={so.x,so.y,so.z},size[3]={z.width,z.height,z.depth};
 [self check:nmb_blit_texture_to_buffer(self.handle,[(NBTexture *)s handle],slice,level,a,size,[(NBBuffer *)d handle],o,row,image) what:"copyFromTexture:toBuffer:"];
}
- (void)generateMipmapsForTexture:(id<MTLTexture>)t {
 if(capturing())capture([NSString stringWithFormat:@"mipmaps %@",capTexture(t)]);
 if(![self textures:NB_OBJECTS(t) count:1])return;
 [self check:nmb_blit_generate_mipmaps(self.handle,[(NBTexture *)t handle]) what:"generateMipmapsForTexture"];
}
- (void)synchronizeResource:(id<MTLResource>)r {(void)r;}
@end

static void completed(void *context);
static void scheduled(void *context);
@implementation NBCommandBuffer
// Metal's SPI dictionary on a command buffer (iOS/macOS 26 QuartzCore stores its frame information in it).
- (NSMutableDictionary *)userDictionary {static char key;NSMutableDictionary *d=objc_getAssociatedObject(self,&key);
 if(!d){d=[NSMutableDictionary dictionary];objc_setAssociatedObject(self,&key,d,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}return d;}
// iOS/macOS 26: the render server attributes a command buffer's GPU time to its clients; the kernel driver attributes by task.
- (void)setResponsibleTaskIDs:(const unsigned *)ids count:(NSUInteger)count {(void)ids;(void)count;}
- (instancetype)init {
 if((self=[super init])){_scheduledHandlers=[NSMutableArray array];_completedHandlers=[NSMutableArray array];_surfaces=[NSMutableSet set];_resourceOwners=[NSMutableSet set];_handlers=dispatch_group_create();_schedulingHandlers=dispatch_group_create();}
 if(self)createdUs=nowUs();
 return self;
}
- (void)dealloc {if(_holdsSlot)dispatch_semaphore_signal(_queue.slots);nmb_command_buffer_release(_handle);}
- (void)giveSlot {@synchronized(self){if(_holdsSlot){_holdsSlot=NO;dispatch_semaphore_signal(_queue.slots);}}}
- (id<MTLCommandQueue>)commandQueue {return (id<MTLCommandQueue>)_queue;}
- (BOOL)retainedReferences {return YES;}
// Protected content is not supported (the runtime's AppleOnlyCall::SetProtectionOptions takes Metal's default only): any other option fails
// the command buffer.
- (uint64_t)protectionOptions {return 0;}
- (void)setProtectionOptions:(uint64_t)options {if(options)[self refuse:[NSString stringWithFormat:@"protection options 0x%llx",options]];}
- (void)refuse:(NSString *)what {
 if(!self.encodeFailure){self.encodeFailure=what;NBLOG(@"native-metal: command buffer refused: %@",what);}
}
- (MTLCommandBufferStatus)status {
 if(self.failedLocally)return MTLCommandBufferStatusError;
 return (MTLCommandBufferStatus)nmb_command_buffer_status(_handle);
}
- (NSError *)error {
 if(self.failedLocally)return nbError(self.encodeFailure);
 if(self.status!=MTLCommandBufferStatusError)return nil;
 char text[512]={};nmb_command_buffer_error(_handle,text,sizeof text);
 return nbError([NSString stringWithUTF8String:text]);
}
// Debug groups and cache hints name work for tools and the driver's caches; they change nothing the GPU computes here.
- (void)pushDebugGroup:(NSString *)name {(void)name;}
- (void)popDebugGroup {}
- (void)encodeCacheHintTag:(uint64_t)tag resourceGroups:(const id __unsafe_unretained [])groups count:(NSUInteger)count {(void)tag;(void)groups;(void)count;}
- (void)encodeCacheHintFinalize:(uint64_t)tag resourceGroups:(const id __unsafe_unretained [])groups count:(NSUInteger)count {(void)tag;(void)groups;(void)count;}
// The kernel driver's own submission times are not recorded: 0 is Metal's answer where they are unavailable.
- (CFTimeInterval)kernelStartTime {return 0;}
- (CFTimeInterval)kernelEndTime {return 0;}
- (BOOL)commitAndWaitUntilSubmitted {[(id<MTLCommandBuffer>)self commit];[self waitUntilScheduled];return !self.failedLocally;}
- (CFTimeInterval)GPUStartTime {return nmb_command_buffer_gpu_time(_handle,false);}
- (CFTimeInterval)GPUEndTime {return nmb_command_buffer_gpu_time(_handle,true);}
- (id)encoder:(NBEncoder *)encoder handle:(NmbEncoder *)handle what:(NSString *)what {
 if(self.committed){[self refuse:[what stringByAppendingString:@" after commit"]];return nil;}
 /* A refused encoder is still an object (UniXNU AdrenoMetal, 10-25): its calls fail on the null handle (check: refuses the command buffer
  * once) and the buffer completes with an error. RenderBox caches the encoder's IMPs with methodForSelector:, and on nil it called
  * address 0 (sheng i568: WidgetRenderer_Default crashed eight launches running, then hit its open cap and lost the device). */
 if(!handle){[self refuse:[NSString stringWithFormat:@"%@: %@",what,lastError()]];encoder.commandBuffer=self;return encoder;}
 encoder.handle=handle;encoder.commandBuffer=self;return encoder;
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoder {
 return [self encoder:[NBComputeEncoder new] handle:self.committed?nullptr:nmb_compute_encoder_new(_handle) what:@"computeCommandEncoder"];
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)type {
 if(type!=MTLDispatchTypeSerial){[self refuse:@"concurrent compute dispatch"];return nil;}
 return [self computeCommandEncoder];
}
- (id<MTLBlitCommandEncoder>)blitCommandEncoder {
 gBlits++;encodedWork.blits++;
 return [self encoder:[NBBlitEncoder new] handle:self.committed?nullptr:nmb_blit_encoder_new(_handle) what:@"blitCommandEncoder"];
}
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d {
 {id<MTLTexture> target=d.colorAttachments[0].texture;   // dada: the surface probe's target (finish)
  if([(id)target isKindOfClass:[NBTexture class]] && [(NBTexture *)target surface])objc_setAssociatedObject(self,&gSurfaceProbeKey,target,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}
 if(capturing()) {
  NSMutableString *line=[NSMutableString stringWithFormat:@"pass rt %lux%lu",(unsigned long)d.renderTargetWidth,(unsigned long)d.renderTargetHeight];
  for(unsigned i=0;i<4;i++) {
   MTLRenderPassColorAttachmentDescriptor *c=d.colorAttachments[i];if(!c.texture)continue;
   [line appendFormat:@" | c%u %@ lv%lu sl%lu load%lu store%lu clear %.2f,%.2f,%.2f,%.2f%@",i,capTexture(c.texture),(unsigned long)c.level,
    (unsigned long)c.slice,(unsigned long)c.loadAction,(unsigned long)c.storeAction,c.clearColor.red,c.clearColor.green,c.clearColor.blue,
    c.clearColor.alpha,c.resolveTexture?[@" resolve " stringByAppendingString:capTexture(c.resolveTexture)]:@""];
  }
  if(d.depthAttachment.texture)[line appendFormat:@" | depth %@ load%lu",capTexture(d.depthAttachment.texture),(unsigned long)d.depthAttachment.loadAction];
  if(d.stencilAttachment.texture)[line appendFormat:@" | stencil %@ load%lu",capTexture(d.stencilAttachment.texture),(unsigned long)d.stencilAttachment.loadAction];
  capture(line);
 }
 NmbRenderPassDescriptor pass={};
 {
  id<MTLTexture> first=d.colorAttachments[0].texture;
  if(first) {
   const uint64_t pixels=(uint64_t)first.width*first.height;
   gPasses++;gPassPixels+=pixels;encodedWork.passes++;encodedWork.pixels+=pixels;
   uint64_t seen=gBiggestPass.load();while(pixels>seen && !gBiggestPass.compare_exchange_weak(seen,pixels)){}
   if(pixels>=2000000)gFullScreenPasses++;
   // dada: by the first attachment's format and its load action (which formats the frames' pixels go to, g2k28: some 1 Gpixel/s)
   {std::lock_guard<std::mutex> lock(gFormatLock);
    FormatWork &w=gFormatWork[(uint64_t)first.pixelFormat<<8|(uint64_t)d.colorAttachments[0].loadAction<<4|(first.sampleCount>1?1:0)];
    w.passes++;w.pixels+=pixels;}
  }
 }
 auto attachment=[&](MTLRenderPassAttachmentDescriptor *a,NmbPassAttachment &out)->bool {
  if(!a.texture)return true;
  if(![(id)a.texture isKindOfClass:[NBTexture class]])return false;
  NBTexture *texture=(NBTexture *)a.texture;
  if(gSingleSample && a.resolveTexture && [(id)a.resolveTexture isKindOfClass:[NBTexture class]]) {
   // (single-sampled: the pass draws into the resolve texture itself, stored)
   NBTexture *resolve=(NBTexture *)a.resolveTexture;if(resolve.surface)[_surfaces addObject:resolve];
   [_resourceOwners addObject:resolve];[_resourceOwners addObject:texture];
   out.texture=resolve.handle;out.level=a.resolveLevel;out.slice=a.resolveSlice;out.depth_plane=a.resolveDepthPlane;out.load=a.loadAction;
   out.store=MTLStoreActionStore;
   if(resolve.surface && a==d.colorAttachments[0])objc_setAssociatedObject(self,&gSurfaceProbeKey,resolve,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
   return true;
  }
  if(texture.surface)[_surfaces addObject:texture];
  [_resourceOwners addObject:texture];
  out.texture=texture.handle;out.level=a.level;out.slice=a.slice;out.depth_plane=a.depthPlane;out.load=a.loadAction;out.store=a.storeAction;
  if(gSingleSample && (out.store==MTLStoreActionMultisampleResolve || out.store==MTLStoreActionStoreAndMultisampleResolve))
   out.store=MTLStoreActionStore;
  // dada: the resolve texture (left out before: no multisampled pass resolved, the wallpaper's drawable stayed empty, g2k21)
  if(a.resolveTexture) {
   if(![(id)a.resolveTexture isKindOfClass:[NBTexture class]])return false;
   NBTexture *resolve=(NBTexture *)a.resolveTexture;if(resolve.surface)[_surfaces addObject:resolve];
   [_resourceOwners addObject:resolve];
   out.resolve=resolve.handle;out.resolve_level=a.resolveLevel;out.resolve_slice=a.resolveSlice;out.resolve_depth_plane=a.resolveDepthPlane;
   if(resolve.surface && a==d.colorAttachments[0])objc_setAssociatedObject(self,&gSurfaceProbeKey,resolve,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  }
  return true;
 };
 for(unsigned i=0;i<8;i++) {
  MTLRenderPassColorAttachmentDescriptor *c=d.colorAttachments[i];
  if(!attachment(c,pass.colors[i])){[self refuse:@"foreign color attachment"];return nil;}
  pass.colors[i].clear[0]=c.clearColor.red;pass.colors[i].clear[1]=c.clearColor.green;pass.colors[i].clear[2]=c.clearColor.blue;pass.colors[i].clear[3]=c.clearColor.alpha;
 }
 if(!attachment(d.depthAttachment,pass.depth) || !attachment(d.stencilAttachment,pass.stencil)){[self refuse:@"foreign depth/stencil attachment"];return nil;}
 pass.depth.clear[0]=d.depthAttachment.clearDepth;pass.stencil.clear[0]=d.stencilAttachment.clearStencil;
 pass.width=d.renderTargetWidth;pass.height=d.renderTargetHeight;pass.array_length=d.renderTargetArrayLength;
 pass.samples=gSingleSample?(d.defaultRasterSampleCount?1:0):d.defaultRasterSampleCount;
 NBBuffer *probeVisibility=nil;   // data-a-live: a visibility buffer of the probe's own for this pass
 if(pandoraAstcProbe() && !d.visibilityResultBuffer && gDeviceObject) {
  probeVisibility=(NBBuffer *)[(id<MTLDevice>)gDeviceObject newBufferWithLength:64*8 options:MTLResourceStorageModeShared];
  if(probeVisibility) {
   memset(probeVisibility.contents,0,64*8);
   [_resourceOwners addObject:probeVisibility];
   pass.visibility=probeVisibility.handle;
  }
 }
 if(d.visibilityResultBuffer) {
  if(![(id)d.visibilityResultBuffer isKindOfClass:[NBBuffer class]]){[self refuse:@"foreign visibility result buffer"];return nil;}
  [_resourceOwners addObject:d.visibilityResultBuffer];
  pass.visibility=[(NBBuffer *)d.visibilityResultBuffer handle];
 }
 id<MTLRenderCommandEncoder> made=[self encoder:[NBRenderEncoder new] handle:self.committed?nullptr:nmb_render_encoder_new(_handle,&pass) what:@"renderCommandEncoder"];
 if(made && pandoraAstcProbe() && d.colorAttachments[0].texture)   // data-a-live: the pass's target, for the readback
  objc_setAssociatedObject(made,&gProbePassTargetKey,d.colorAttachments[0].texture,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
 if(made && probeVisibility)objc_setAssociatedObject(made,&gProbeVisKey,probeVisibility,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
 if(made && pandoraAstcProbe()) {   // data-a-live: the pass, for the probe's draw log
  NSMutableString *pass=[NSMutableString stringWithFormat:@"rt %lux%lu",(unsigned long)d.renderTargetWidth,(unsigned long)d.renderTargetHeight];
  for(unsigned i=0;i<8;i++) {
   MTLRenderPassColorAttachmentDescriptor *c=d.colorAttachments[i];if(!c.texture)continue;
   [pass appendFormat:@" | c%u %@ load%lu store%lu clear %g,%g,%g,%g",i,capTexture(c.texture),(unsigned long)c.loadAction,
    (unsigned long)c.storeAction,c.clearColor.red,c.clearColor.green,c.clearColor.blue,c.clearColor.alpha];
  }
  if(d.depthAttachment.texture)[pass appendFormat:@" | depth %@",capTexture(d.depthAttachment.texture)];
  if(d.stencilAttachment.texture)[pass appendFormat:@" | stencil %@",capTexture(d.stencilAttachment.texture)];
  probeBindings(made)[@"pass"]=pass;
 }
 return made;
}
- (void)addScheduledHandler:(MTLCommandBufferHandler)block {if(block)@synchronized(self){[_scheduledHandlers addObject:[block copy]];}}
- (void)addCompletedHandler:(MTLCommandBufferHandler)block {if(block)@synchronized(self){[_completedHandlers addObject:[block copy]];}}
// A drawable (RenderBox's CAMetalLayer path) is presented once the GPU has written it: this runtime's scheduling comes before completion.
- (void)presentDrawable:(id<MTLDrawable>)drawable {[self addCompletedHandler:^(id<MTLCommandBuffer>){[drawable present];}];}
- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)time {[self addCompletedHandler:^(id<MTLCommandBuffer>){[drawable presentAtTime:time];}];}
- (void)enqueue {}
- (void)event:(id<MTLEvent>)event value:(uint64_t)value signal:(bool)signal {
 if(![(id)event isKindOfClass:[NBEvent class]]){[self refuse:@"foreign event"];return;}
 if(self.committed){[self refuse:@"event encoded after commit"];return;}
 NmbEvent *handle=[(NBEvent *)event handle];
 if(!(signal?nmb_command_buffer_signal_event(_handle,handle,value):nmb_command_buffer_wait_event(_handle,handle,value)))
  [self refuse:[NSString stringWithFormat:@"%s: %@",signal?"encodeSignalEvent":"encodeWaitForEvent",lastError()]];
}
- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value {[self event:event value:value signal:true];}
- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value {[self event:event value:value signal:false];}
// The runtime has submitted the work. Deliver scheduling without waiting for GPU completion.
- (void)didSchedule {
 {
  const uint64_t took=nowUs()-self.committedUs;
  std::lock_guard<std::mutex> lock(gCommitLock);
  CommitRecord &r=gCommitRing[self.commitIndex%32];
  if(r.index==self.commitIndex)r.scheduledUs=took;
 }
 NSArray *handlers;
 @synchronized(self){
  _schedulingDelivered=YES;handlers=[_scheduledHandlers copy];[_scheduledHandlers removeAllObjects];
 }
 void (^run)(void)=^{
  for(MTLCommandBufferHandler block in handlers)block((id<MTLCommandBuffer>)self);
  dispatch_group_leave(self->_schedulingHandlers);
 };
 // Run on the runtime's scheduled-handler thread, preserving its ordering and own-wait
 // deadlock checks. The queue's completionQueue routes completed handlers only.
 run();
}
// After completion (or a refusal): invalidate the CPU's view of imported surfaces, then completed handlers, once.
- (void)finish {
 [self giveSlot];
 flushSurfaces(_surfaces);
 flushWrapped(_resourceOwners);   // data-a-live: no-copy memory too
 if(NBTexture *target=objc_getAssociatedObject(self,&gSurfaceProbeKey))
  surfaceProbe(target,self.failedLocally||self.encodeFailure,objc_getAssociatedObject(self,&gSurfaceSampledKey));
 // Actual GPU completion (or a local refusal that never submitted) is the
 // release point. A retained completed command must not pin every old frame.
 [_surfaces removeAllObjects];[_resourceOwners removeAllObjects];
 @synchronized(gInFlight){[gInFlight removeObject:self];}
 static std::atomic<int> visTold{0};
 for(NSArray *read in objc_getAssociatedObject(self,&gProbeVisKey)) {   // data-a-live: samples counted per draw
  if(visTold.fetch_add(1)>=8)break;
  const uint64_t *counts=(const uint64_t *)[(id<MTLBuffer>)read[0] contents];NSArray *labels=read[1];
  for(NSUInteger i=0;i<labels.count && counts;i++)
   probeLog([NSString stringWithFormat:@"visibility %d slot %lu",visTold.load(),(unsigned long)i],
            [NSString stringWithFormat:@"%llu samples",(unsigned long long)counts[i]],labels[i]);
 }
 if(NSArray *watch=objc_getAssociatedObject(self,&gProbeWatchKey)) {   // data-a-live: after completion, CPU and GPU views
  id<MTLCommandQueue> queue=(id<MTLCommandQueue>)self.queue;
  for(NSArray *w in watch) {
   id<MTLBuffer> buffer=w[0];const NSUInteger offset=[w[1] unsignedLongValue];NSString *label=w[2];
   const uint8_t *c=(const uint8_t *)buffer.contents;
   NSLog(@"pandora-metal watch %@ done, CPU: %@",label,c && offset<buffer.length?probeHex(c+offset,buffer.length-offset,64):@"(no contents)");
   dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
    const NSUInteger n=MIN((NSUInteger)64,buffer.length-offset);
    id<MTLBuffer> copy=[buffer.device newBufferWithLength:64 options:MTLResourceStorageModeShared];
    memset(copy.contents,0xa5,64);
    id<MTLCommandBuffer> cb=[queue commandBuffer];id<MTLBlitCommandEncoder> b=[cb blitCommandEncoder];
    [b copyFromBuffer:buffer sourceOffset:offset toBuffer:copy destinationOffset:0 size:n];
    [b endEncoding];[cb commit];[cb waitUntilCompleted];
    NSLog(@"pandora-metal watch %@ done, GPU: %@ (status %ld)",label,probeHex(copy.contents,n,64),(long)cb.status);
   });
  }
 }
 if(NSSet *targets=objc_getAssociatedObject(self,&gProbeReadbackKey)) {   // data-a-live: the App Library readback
  static std::atomic<int> readbacks{0};
  id<MTLCommandQueue> queue=(id<MTLCommandQueue>)self.queue;
  for(id<MTLTexture> target in [targets copy]) {
   if(readbacks.fetch_add(1)>=12)break;
   dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
    const NSUInteger w=target.width,h=target.height,format=target.pixelFormat;
    if(format!=MTLPixelFormatBGRA8Unorm && format!=MTLPixelFormatBGRA8Unorm_sRGB && format!=MTLPixelFormatRGBA8Unorm) {
     NSLog(@"pandora-metal astc readback: target %lux%lu format %lu, not read",(unsigned long)w,(unsigned long)h,(unsigned long)format);return;
    }
    std::vector<uint8_t> pixels(w*h*4,0xa5);
    if(target.storageMode==MTLStorageModePrivate) {
     // Private: the GPU copies it into a shared buffer first (bb33: getBytes refuses private textures).
     id<MTLBuffer> staging=[target.device newBufferWithLength:w*h*4 options:MTLResourceStorageModeShared];
     id<MTLCommandBuffer> copy=[queue commandBuffer];
     id<MTLBlitCommandEncoder> blit=[copy blitCommandEncoder];
     [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(w,h,1)
                  toBuffer:staging destinationOffset:0 destinationBytesPerRow:w*4 destinationBytesPerImage:w*h*4];
     [blit endEncoding];[copy commit];[copy waitUntilCompleted];
     if(staging.contents)memcpy(pixels.data(),staging.contents,w*h*4);
    } else
    [target getBytes:pixels.data() bytesPerRow:w*4 fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
    const bool bgra=format!=MTLPixelFormatRGBA8Unorm;
    NSLog(@"pandora-metal astc readback: target %lux%lu format %lu storage %lu%s",(unsigned long)w,(unsigned long)h,(unsigned long)format,
          (unsigned long)target.storageMode,[(NBTexture *)target surface]?" surface":"");
    for(unsigned gy=0;gy<8;gy++) {
     NSMutableString *line=[NSMutableString string];
     for(unsigned gx=0;gx<8;gx++) {
      uint64_t sum[4]={},n=0;
      for(NSUInteger y=gy*h/8;y<(gy+1)*h/8;y+=4)for(NSUInteger x=gx*w/8;x<(gx+1)*w/8;x+=4) {
       const uint8_t *p=&pixels[(y*w+x)*4];
       sum[0]+=p[bgra?2:0];sum[1]+=p[1];sum[2]+=p[bgra?0:2];sum[3]+=p[3];n++;
      }
      if(!n)n=1;
      [line appendFormat:@" %02x%02x%02x%02x",(unsigned)(sum[0]/n),(unsigned)(sum[1]/n),(unsigned)(sum[2]/n),(unsigned)(sum[3]/n)];
     }
     NSLog(@"pandora-metal astc readback r%u:%@",gy,line);
    }
   });
  }
 }
 NSArray *completedHandlers;
 @synchronized(self){
  // A frontend refusal or a failed submission never scheduled. Unblock its waiter without inventing a scheduled event.
  if(!_schedulingDelivered){_schedulingDelivered=YES;[_scheduledHandlers removeAllObjects];dispatch_group_leave(_schedulingHandlers);}
  completedHandlers=[_completedHandlers copy];[_completedHandlers removeAllObjects];
 }
 void (^run)(void)=^{
  for(MTLCommandBufferHandler block in completedHandlers)block((id<MTLCommandBuffer>)self);
  dispatch_group_leave(self->_handlers);
 };
 dispatch_queue_t queue=_queue.completionQueue;
 if(queue)dispatch_async(queue,run);else run();
}
// dada: NB_MIN_COMMIT_MS=N (the plug-in sets 100 for the posters): this process's commits at least N ms apart. The lock screen's wallpaper
// drew frames of 40-55 ms of GPU even single-sampled (g2k32), and the render server's frames queue behind each on the one ring.
static uint64_t minCommitUs() {
 static const uint64_t us=[]{const char *v=getenv("NB_MIN_COMMIT_MS");return v?(uint64_t)strtoull(v,nullptr,10)*1000:0;}();
 return us;
}
- (void)commit {
 if(self.committed){NBLOG(@"native-metal: commit twice");return;}
 if(const uint64_t gap=minCommitUs()) {
  static std::mutex paced;static uint64_t lastUs;
  std::lock_guard<std::mutex> lock(paced);
  const uint64_t now=nowUs();
  if(lastUs && now-lastUs<gap)usleep((useconds_t)(gap-(now-lastUs)));
  lastUs=nowUs();
 }
 syncMirrors(); // Preserve commit-time coherence for all mirrored imports.
 if(capturing())capture(@"commit");
 self.committed=YES;
 self.committedUs=nowUs();
 {
  std::lock_guard<std::mutex> lock(gCommitLock);
  // Assign once under the same lock as publication. A concurrent commit cannot reuse this index.
  const uint32_t index=(uint32_t)(gCommits.fetch_add(1)+1);self.commitIndex=index;
  CommitRecord &r=gCommitRing[index%32];
  r={};r.index=index;r.committedUs=self.committedUs;r.ageUs=self.committedUs-createdUs;r.work=encodedWork;
 }
 dispatch_group_enter(_handlers);
 dispatch_group_enter(_schedulingHandlers);
 @synchronized(gInFlight){[gInFlight addObject:self];}
 if(!self.encodeFailure) {
  flushSurfaces(_surfaces);
  flushWrapped(_resourceOwners);   // data-a-live: no-copy memory too
  for(NSArray *w in objc_getAssociatedObject(self,&gProbeWatchKey)) {   // data-a-live: the probe's buffers at commit
   id<MTLBuffer> buffer=w[0];const NSUInteger offset=[w[1] unsignedLongValue];const uint8_t *c=(const uint8_t *)buffer.contents;
   // bb39: QuartzCore writes these buffers after binding them; what the GPU reads is what they hold at commit, so the
   // replay takes its bytes from here (1 KiB from the offset, in pieces).
   probeLog([NSString stringWithFormat:@"commit %@",w[2]],@"bytes",c && offset<buffer.length?probeHex(c+offset,buffer.length-offset,1024):@"(no contents)");
  }
  // Completion owns this reference; the runtime orders the non-owning scheduled callback before it.
  void *context=(__bridge_retained void *)self;
  if(nmb_command_buffer_on_completed(_handle,completed,context) &&
     nmb_command_buffer_on_scheduled(_handle,scheduled,context) && nmb_command_buffer_commit(_handle))return;
  (void)CFBridgingRelease(context);   // refused registration/commit cannot call the registered handlers
  [self refuse:[@"commit: " stringByAppendingString:lastError()]];
 }
 self.failedLocally=YES;gCommitFailures++;
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{[self finish];});
}
- (void)waitUntilCompleted {
 if(!self.committed)return;
 const uint64_t began=nowUs();
 if(!self.failedLocally)nmb_command_buffer_wait(_handle,0);
 dispatch_group_wait(_handlers,DISPATCH_TIME_FOREVER);
 gTimeWaitCompleted.add(began);
}
- (void)waitUntilScheduled {
 if(!self.committed)return;
 const uint64_t began=nowUs();
 if(!self.failedLocally && !nmb_command_buffer_wait_scheduled(_handle)){
  NBLOG(@"native-metal: waitUntilScheduled refused: %@",lastError());return;
 }
 dispatch_group_wait(_schedulingHandlers,DISPATCH_TIME_FOREVER);
 gTimeWaitScheduled.add(began);
}
@end
static void scheduled(void *context) {[(__bridge NBCommandBuffer *)context didSchedule];}
// The runtime's completion: the command buffer's own reference, taken at commit, is given back here.
static void completed(void *context) {
 gCompleted++;
 NBCommandBuffer *commandBuffer=(__bridge_transfer NBCommandBuffer *)context;
 {
  const uint64_t took=nowUs()-commandBuffer.committedUs;
  gTimeEncodeToCommit.count++;gTimeEncodeToCommit.total+=took;
  uint64_t seen=gTimeEncodeToCommit.slowest.load();while(took>seen && !gTimeEncodeToCommit.slowest.compare_exchange_weak(seen,took)){}
  std::lock_guard<std::mutex> lock(gCommitLock);
  CommitRecord &r=gCommitRing[commandBuffer.commitIndex%32];
  if(r.index==commandBuffer.commitIndex)r.callbackUs=took;
 }
 [commandBuffer finish];
}

@implementation NBQueue
- (void)dealloc {nmb_queue_release(_handle);}
- (id<MTLCommandBuffer>)commandBuffer {
 // dada: as Metal's queue does, at most so many command buffers unfinished: a new one waits for one to complete. Without it the render
 // server committed faster than the GPU finished, and every unfinished buffer held its textures and command streams until the
 // phone ran out of memory (g2k6: 2.6 GB in the render server, then the phone froze). 16 as a ceiling; a wait of over 2 s goes on
 // anyway (a stuck GPU must not stop the caller for ever).
 @synchronized(self){if(!_slots)_slots=dispatch_semaphore_create(16);}
 const bool slot=dispatch_semaphore_wait(_slots,dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC))==0;
 if(!slot){static std::atomic<unsigned> said;if(said++<5)consoleLine(@"16 command buffers unfinished for 2 s: one more made anyway");}
 NmbCommandBuffer *handle=nmb_command_buffer_new(_handle);
 if(!handle){if(slot)dispatch_semaphore_signal(_slots);NBLOG(@"native-metal: commandBuffer refused: %@",lastError());return nil;}
 NBCommandBuffer *commandBuffer=[NBCommandBuffer new];commandBuffer.handle=handle;commandBuffer.queue=self;commandBuffer.holdsSlot=slot;
 return (id<MTLCommandBuffer>)commandBuffer;
}
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences {return [self commandBuffer];}
- (void)setSubmissionQueue:(dispatch_queue_t)queue {(void)queue;}   // submission happens on the caller's thread here
// setCompletionQueue: is the property's setter.
// GPU priorities are not applied (one hardware queue): reported as not applied.
- (BOOL)setGPUPriority:(NSUInteger)priority {(void)priority;return NO;}
- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority {(void)priority;return NO;}
// What a new Metal queue reports (MTLIOAccelCommandQueue: normal 1, background 3). RenderBox and Core Image read both before any frame.
- (NSUInteger)getGPUPriority {return 1;}
- (NSUInteger)getBackgroundGPUPriority {return 3;}
- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)d {(void)d;return [self commandBuffer];}
@end

// ---- the device ----
template <typename T> static T failWith(NSError **error,NSString *what,T value) {
 // dada: logged at once as well: the render server aborts on a failed pipeline long before the telemetry's next row (g2vk4)
 NSLog(@"gpu2-metal: %@ failed: %@",what,lastError());consoleLine([NSString stringWithFormat:@"%@ failed: %@",what,lastError()]);
 noteProblem([NSString stringWithFormat:@"%@: %@",what,lastError()]);
 if(error)*error=nbError([NSString stringWithFormat:@"%@: %@",what,lastError()]);
 return value;
}
// Which of QuartzCore's paths asked for a pipeline (int-153: every one was made synchronously on its render thread and no generic pipeline
// was ever asked for): its return addresses as offsets from its image header (header 0x189429000 in the 21H461 cache, so offset +
// 0x189429000 is the address in that image's disassembly), the global feature flags it ORs into every context's flags, and its
// CA_DISABLE_GENERIC_SHADERS, CA_FORCE_GENERIC_SHADERS and CA_DISABLE_PRECOMPILED_PIPELINES debug options (ca_debug_options +0x55..0x57,
// read only where dladdr places that address in QuartzCore).
static std::string quartzCoreCallers() {
 void *frames[24];const int n=backtrace(frames,24);
 std::string out="qc";char item[32];unsigned kept=0;uintptr_t header=0;
 for(int i=0;i<n && kept<8;i++) {
  Dl_info info{};
  if(!dladdr(frames[i],&info) || !info.dli_fname || !strstr(info.dli_fname,"/QuartzCore"))continue;
  header=(uintptr_t)info.dli_fbase;
  snprintf(item,sizeof item," %lx",(unsigned long)((uintptr_t)frames[i]-header));out+=item;++kept;
 }
 // The context's own flags (int-154: the draws never took the generic path, which only bit 18 of MetalContext +0x1a0 or the debug byte
 // can cause, and the debug byte was 0). MetalContext::create_pipeline_state (header +0x143000) saves its caller's x19 at fp-8, and its
 // caller MetalContext::draw keeps `this` in x19: the frame record whose return address is +0x143214 links to that frame. Read with
 // vm_read so a different stack layout reports ctx=? instead of faulting.
 if(header) {
  uintptr_t fp=(uintptr_t)__builtin_frame_address(0),ctx=0;
  for(unsigned depth=0;fp && depth<24;++depth) {
   uintptr_t record[2]={};vm_size_t got=0;
   if(vm_read_overwrite(mach_task_self(),fp,sizeof record,(vm_address_t)record,&got)!=KERN_SUCCESS || got!=sizeof record)break;
   if(record[1]-header==0x143214) {
    uintptr_t saved=0;
    if(record[0] && vm_read_overwrite(mach_task_self(),record[0]-8,sizeof saved,(vm_address_t)&saved,&got)==KERN_SUCCESS)ctx=saved;
    break;
   }
   if(record[0]<=fp)break;
   fp=record[0];
  }
  uint32_t flags[2]={};vm_size_t got=0;
  if(ctx && vm_read_overwrite(mach_task_self(),ctx+0x1a0,4,(vm_address_t)&flags[0],&got)==KERN_SUCCESS &&
     vm_read_overwrite(mach_task_self(),ctx+0xb3c,4,(vm_address_t)&flags[1],&got)==KERN_SUCCESS) {
   snprintf(item,sizeof item," ctx=%x/%x",flags[0],flags[1]);out+=item;
  } else out+=" ctx=?";
 }
 static auto global=(uint32_t (*)(void))dlsym(RTLD_DEFAULT,"CARenderGetGlobalFeatureFlags");
 snprintf(item,sizeof item," global=%x",global?global():0xffffffffu);out+=item;
 if(header) {
  const uintptr_t options=header+(0x1f491b3c5ull-0x189429000ull);
  Dl_info info{};
  if(dladdr((void *)options,&info) && info.dli_fname && strstr(info.dli_fname,"/QuartzCore") && (uintptr_t)info.dli_fbase==header) {
   const volatile uint8_t *b=(const volatile uint8_t *)options;
   snprintf(item,sizeof item," dbg=%u%u%u",b[0],b[1],b[2]);out+=item;
  } else out+=" dbg=?";
 }
 return out;
}
// What a creation cost beyond its wall time (int-156: a translation the host does in 6 ms took 665 ms on the tablet, whose single core is
// as fast): this thread's CPU time (user and system, microseconds) and the task's page faults, page-ins and context switches. A creation
// whose CPU time is near its wall time computed; one whose faults are many paid the 4 KiB fault path; one far below it waited.
struct NBCost {
 uint64_t user=0,system=0;int64_t faults=0,pageins=0,switches=0;
 static NBCost now() {
  NBCost c;thread_basic_info_data_t t{};mach_msg_type_number_t n=THREAD_BASIC_INFO_COUNT;mach_port_t self=mach_thread_self();
  if(thread_info(self,THREAD_BASIC_INFO,(thread_info_t)&t,&n)==KERN_SUCCESS) {
   c.user=(uint64_t)t.user_time.seconds*1000000+t.user_time.microseconds;c.system=(uint64_t)t.system_time.seconds*1000000+t.system_time.microseconds;
  }
  mach_port_deallocate(mach_task_self(),self);
  task_events_info_data_t e{};n=TASK_EVENTS_INFO_COUNT;
  if(task_info(mach_task_self(),TASK_EVENTS_INFO,(task_info_t)&e,&n)==KERN_SUCCESS){c.faults=e.faults;c.pageins=e.pageins;c.switches=e.csw;}
  return c;
 }
 std::string since(const NBCost &b) const {
  char text[128];snprintf(text,sizeof text," cpu=%llu sys=%llu flt=%lld pin=%lld csw=%lld",(unsigned long long)(user+system-b.user-b.system),
      (unsigned long long)(system-b.system),(long long)(faults-b.faults),(long long)(pageins-b.pageins),(long long)(switches-b.switches));
  return text;
 }
};
// One journal line of a render pipeline creation begun at `began` (microseconds), `refused` its refusal or null. The label is the
// caller's name for the pipeline (QuartzCore's: its spec's encoded name).
static void journalRenderPipeline(MTLRenderPipelineDescriptor *d,uint64_t began,const char *refused,const std::string &cost) {
 if(gJournalPath.empty())return;
 const uint64_t ended=nowUs();uint64_t tid=0;pthread_threadid_np(nullptr,&tid);
 char head[256];
 snprintf(head,sizeof head,"R t=%llu tid=%llu main=%d us=%llu ok=%d samples=%lu topo=%lu ds=%lu/%lu",(unsigned long long)((began-gJournalBegan)/1000),
     (unsigned long long)tid,pthread_main_np(),(unsigned long long)(ended-began),refused?0:1,(unsigned long)d.rasterSampleCount,
     (unsigned long)d.inputPrimitiveTopology,(unsigned long)d.depthAttachmentPixelFormat,(unsigned long)d.stencilAttachmentPixelFormat);
 std::string line=head;
 line+=cost;
 line+=" label=";line+=d.label.length?d.label.UTF8String:"-";
 for(unsigned i=0;i<8;i++) {
  MTLRenderPipelineColorAttachmentDescriptor *a=d.colorAttachments[i];if(a.pixelFormat==MTLPixelFormatInvalid)continue;
  snprintf(head,sizeof head," c%u=%lu:%lu:%d:%lu:%lu:%lu:%lu:%lu:%lu",i,(unsigned long)a.pixelFormat,(unsigned long)a.writeMask,(int)a.blendingEnabled,
      (unsigned long)a.sourceRGBBlendFactor,(unsigned long)a.destinationRGBBlendFactor,(unsigned long)a.rgbBlendOperation,(unsigned long)a.sourceAlphaBlendFactor,
      (unsigned long)a.destinationAlphaBlendFactor,(unsigned long)a.alphaBlendOperation);
  line+=head;
 }
 if(MTLVertexDescriptor *v=d.vertexDescriptor)for(unsigned i=0;i<31;i++) {
  MTLVertexAttributeDescriptor *a=v.attributes[i];if(a.format==MTLVertexFormatInvalid)continue;
  MTLVertexBufferLayoutDescriptor *l=v.layouts[a.bufferIndex<31?a.bufferIndex:0];
  snprintf(head,sizeof head," a%u=%lu:%lu:%lu:%lu:%lu:%lu",i,(unsigned long)a.format,(unsigned long)a.offset,(unsigned long)a.bufferIndex,(unsigned long)l.stride,
      (unsigned long)l.stepFunction,(unsigned long)l.stepRate);
  line+=head;
 }
 NBFunction *vf=[(id)d.vertexFunction isKindOfClass:[NBFunction class]]?(NBFunction *)d.vertexFunction:nil;
 NBFunction *ff=[(id)d.fragmentFunction isKindOfClass:[NBFunction class]]?(NBFunction *)d.fragmentFunction:nil;
 line+=" | v ";line+=vf?vf.name.UTF8String:"-";line+=" ";line+=vf?vf.constantsText:"";
 line+=" | f ";line+=ff?ff.name.UTF8String:"-";line+=" ";line+=ff?ff.constantsText:"";
 if(refused){line+=" | refused ";line+=refused;}
 line+=" | ";line+=quartzCoreCallers();
 for(char &c:line)if(c=='\n')c=' ';
 journal(line);
}
// A render pipeline descriptor in the bridge's form; false, with the error, for a function of another device.
static bool renderDescriptor(MTLRenderPipelineDescriptor *d,NmbRenderPipelineDescriptor *p,NSError **error) {
 if(![(id)d.vertexFunction isKindOfClass:[NBFunction class]] || (d.fragmentFunction && ![(id)d.fragmentFunction isKindOfClass:[NBFunction class]])) {
  if(error)*error=nbError(@"foreign function");return false;
 }
 p->vertex=[(NBFunction *)d.vertexFunction handle];p->fragment=d.fragmentFunction?[(NBFunction *)d.fragmentFunction handle]:nullptr;
 // dada: the stages' linked functions (RenderBox's custom effects link custom_fn into their fragment function: without it every one
 // failed, "NeedsInput(LinkedFunction { symbol: custom_fn })", and the glass those effects draw was missing)
 {
  auto take=[&](MTLLinkedFunctions *linked,NmbFunction **out,const char **group,uint64_t *count)->bool{
   *count=0;
   if(!linked)return true;
   auto add=[&](id<MTLFunction> f,NSString *name)->bool{
    if(![(id)f isKindOfClass:[NBFunction class]]){if(error)*error=nbError(@"foreign linked function");return false;}
    if(*count>=16){if(error)*error=nbError(@"more than 16 linked functions");return false;}
    group[*count]=name?name.UTF8String:nullptr;   // the descriptor's strings live while it does (the caller's whole call)
    out[(*count)++]=[(NBFunction *)f handle];
    return true;
   };
   for(id<MTLFunction> f in linked.functions)if(!add(f,nil))return false;
   for(id<MTLFunction> f in linked.privateFunctions)if(!add(f,nil))return false;
   for(NSString *name in linked.groups)for(id<MTLFunction> f in linked.groups[name]){
    static std::atomic<unsigned> said;
    if(said++<6)consoleLine([NSString stringWithFormat:@"linked group %@: function %@",name,f.name]);
    if(!add(f,name))return false;
   }
   return true;
  };
  if(!take(d.vertexLinkedFunctions,p->vertex_linked,p->vertex_linked_group,&p->vertex_linked_count) ||
     !take(d.fragmentLinkedFunctions,p->fragment_linked,p->fragment_linked_group,&p->fragment_linked_count))return false;
 }
 for(unsigned i=0;i<8;i++) {
  MTLRenderPipelineColorAttachmentDescriptor *a=d.colorAttachments[i];
  p->colors[i]={(uint64_t)a.pixelFormat,(uint64_t)a.writeMask,(uint64_t)a.blendingEnabled,(uint64_t)a.sourceRGBBlendFactor,(uint64_t)a.destinationRGBBlendFactor,
      (uint64_t)a.rgbBlendOperation,(uint64_t)a.sourceAlphaBlendFactor,(uint64_t)a.destinationAlphaBlendFactor,(uint64_t)a.alphaBlendOperation};
 }
 p->depth_format=d.depthAttachmentPixelFormat;p->stencil_format=d.stencilAttachmentPixelFormat;p->samples=gSingleSample?1:d.rasterSampleCount;
 p->topology=d.inputPrimitiveTopology;p->rasterization=d.rasterizationEnabled;
 if(MTLVertexDescriptor *v=d.vertexDescriptor) {
  for(unsigned i=0;i<31;i++) {
   MTLVertexAttributeDescriptor *a=v.attributes[i];if(a.format==MTLVertexFormatInvalid)continue;
   p->vertex_inputs=1;p->attributes[i]={(uint64_t)a.format,a.offset,a.bufferIndex};
   if(a.bufferIndex<31){MTLVertexBufferLayoutDescriptor *l=v.layouts[a.bufferIndex];p->layouts[a.bufferIndex]={l.stride,(uint64_t)l.stepFunction,l.stepRate};}
  }
 }
 return true;
}

// ---- heaps ----
// Metal heaps over the runtime's: an automatic heap places each resource, a placement heap takes an offset. Resources made from one keep it
// alive (their `heap`); a heap's resources share its storage mode, CPU cache mode and (here) hazard tracking mode.
@interface NBHeap : NBObject
@property(nonatomic) NmbHeap *handle;
@property(nonatomic) MTLResourceOptions options;
@property(nonatomic) MTLHeapType heapType;
@end
@implementation NBHeap
- (void)dealloc {nmb_heap_release(_handle);}
- (NSUInteger)size {return nmb_heap_size(_handle);}
- (NSUInteger)usedSize {return nmb_heap_used_size(_handle);}
- (NSUInteger)currentAllocatedSize {return nmb_heap_allocated_size(_handle);}
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)alignment {return nmb_heap_max_available_size(_handle,alignment);}
- (MTLResourceOptions)resourceOptions {return _options;}
- (MTLStorageMode)storageMode {return (MTLStorageMode)((_options&MTLResourceStorageModeMask)>>MTLResourceStorageModeShift);}
- (MTLCPUCacheMode)cpuCacheMode {return (MTLCPUCacheMode)((_options&MTLResourceCPUCacheModeMask)>>MTLResourceCPUCacheModeShift);}
- (MTLHazardTrackingMode)hazardTrackingMode {return (MTLHazardTrackingMode)((_options&MTLResourceHazardTrackingModeMask)>>MTLResourceHazardTrackingModeShift);}
- (MTLHeapType)type {return _heapType;}
- (uint64_t)protectionOptions {return 0;}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {
 const uint64_t answer=nmb_heap_set_purgeable_state(_handle,state);
 if(!answer)NBLOG(@"native-metal: heap setPurgeableState refused: %@",lastError());
 return (MTLPurgeableState)answer;
}
- (id<MTLBuffer>)bufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(uint64_t)offset {
 NmbBuffer *handle=nmb_heap_new_buffer(_handle,length,options,offset);
 if(!handle){NBLOG(@"native-metal: heap buffer refused: %@",lastError());return nil;}
 NBBuffer *buffer=[NBBuffer new];buffer.handle=handle;buffer.options=options;buffer.parentHeap=(id<MTLHeap>)self;
 buffer.heapOffset=offset==UINT64_MAX?0:offset;return (id<MTLBuffer>)buffer;
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {return [self bufferWithLength:length options:options offset:UINT64_MAX];}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(NSUInteger)offset {
 return [self bufferWithLength:length options:options offset:offset];
}
- (id<MTLTexture>)textureWithDescriptor:(MTLTextureDescriptor *)d offset:(uint64_t)offset {
 NmbTextureDescriptor descriptor;if(!textureDescriptor(d,&descriptor))return nil;
 NmbTexture *handle=nmb_heap_new_texture(_handle,&descriptor,offset);
 if(!handle) {
  // dada: what the heap had and what was asked, the first 40 (backboardd's heaps still refuse a texture every few seconds, g2k25:
  // the frames it was for are not drawn, the flicker)
  static std::atomic<int> told{0};
  if(told.fetch_add(1)<40) {
   uint64_t sa[2]={0,0};nmb_device_heap_texture_size_and_align(gDevice,&descriptor,sa);
   consoleLine([NSString stringWithFormat:@"heap texture refused: %lux%lu pf %lu usage %lu mips %lu (size %llu align %llu) in a heap of %llu, %llu used, "
    "%llu allocated, %llu available at its alignment; type %lu, at %@: %@",(unsigned long)d.width,(unsigned long)d.height,(unsigned long)d.pixelFormat,
    (unsigned long)d.usage,(unsigned long)d.mipmapLevelCount,sa[0],sa[1],nmb_heap_size(_handle),nmb_heap_used_size(_handle),nmb_heap_allocated_size(_handle),
    nmb_heap_max_available_size(_handle,sa[1]?sa[1]:1),(unsigned long)self.heapType,offset==UINT64_MAX?@"any offset":@(offset),lastError()]);
  }
  NBLOG(@"native-metal: heap texture refused: %@",lastError());
  // dada: an automatic heap's texture that does not fit is made on its own instead of refused: CoreAnimation draws nothing where its heap
  // refuses (g2k26: full 25 MB heaps, and some fragmented -- 13 MB asked with 17.6 MB free in pieces of 8 MB), the screen flickering.
  // Such a texture belongs to no heap's range; making it aliasable gives nothing back, and freeing it frees its own memory.
  if(offset==UINT64_MAX && self.heapType!=MTLHeapTypePlacement) {
   MTLTextureDescriptor *own=[d copy];own.resourceOptions=self.options&(MTLResourceStorageModeMask|MTLResourceCPUCacheModeMask);
   if(id<MTLTexture> t=[(id<MTLDevice>)gDeviceObject newTextureWithDescriptor:own]) {
    static std::atomic<uint64_t> apart{0};
    if(apart.fetch_add(1)%50==0)consoleLine([NSString stringWithFormat:@"heap texture made apart (%llu so far)",(unsigned long long)apart.load()]);
    return t;
   }
  }
  return nil;
 }
 NBTexture *texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;
 texture.parentHeap=(id<MTLHeap>)self;texture.heapOffset=offset==UINT64_MAX?0:offset;return (id<MTLTexture>)texture;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d {return [self textureWithDescriptor:d offset:UINT64_MAX];}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset {return [self textureWithDescriptor:d offset:offset];}
@end
// Apple's SPI resource groups name resources for cache hints (encodeCacheHintTag:/Finalize:); a group only keeps its resources alive.
@interface NBResourceGroup : NBObject
@property(nonatomic,strong) NSArray *resources;
@end
@implementation NBResourceGroup
@end

// Creation and archive population must use the same complete supported descriptor.
static bool computeDescriptor(MTLComputePipelineDescriptor *d,NmbComputePipelineDescriptor *p,NSError **error) {
 if(!d || ![(id)d.computeFunction isKindOfClass:[NBFunction class]]){if(error)*error=nbError(@"foreign compute function");return false;}
 // Metal may materialize empty stage/linked descriptors on access. Preserve the
 // default meaning while refusing state this frontend cannot yet represent.
 MTLStageInputOutputDescriptor *stage=d.stageInputDescriptor;
 bool empty=!stage || (stage.indexType==MTLIndexTypeUInt16 && stage.indexBufferIndex==0);
 for(NSUInteger index=0;stage && index<31;++index) {
  MTLAttributeDescriptor *a=stage.attributes[index];MTLBufferLayoutDescriptor *b=stage.layouts[index];
  empty=empty && a.format==MTLAttributeFormatInvalid && !a.offset && !a.bufferIndex && !b.stride &&
        b.stepFunction==MTLStepFunctionPerVertex && b.stepRate==1;
 }
 if(!empty){if(error)*error=nbError(@"compute stage inputs are not implemented");return false;}
 MTLLinkedFunctions *linked=d.linkedFunctions;
 if(linked.functions.count || linked.binaryFunctions.count || linked.privateFunctions.count || linked.groups.count ||
    d.preloadedLibraries.count || d.insertLibraries.count) {
  if(error)*error=nbError(@"compute linked/dynamic libraries are not implemented");return false;
 }
 p->function=[(NBFunction *)d.computeFunction handle];p->max_threads=d.maxTotalThreadsPerThreadgroup;
 p->multiple_of_execution_width=d.threadGroupSizeIsMultipleOfThreadExecutionWidth;p->indirect=d.supportIndirectCommandBuffers;
 p->max_call_stack_depth=d.maxCallStackDepth;p->adding_binary_functions=d.supportAddingBinaryFunctions;
 for(NSUInteger index=0;index<31;++index)p->mutability[index]=d.buffers[index].mutability;
 return true;
}

// ---- binary archives ----
@interface NBBinaryArchive : NBObject
@property(nonatomic) NmbBinaryArchive *handle;
@end
@implementation NBBinaryArchive
- (void)dealloc {nmb_binary_archive_release(_handle);}
- (NSUInteger)options {return 0;}
- (BOOL)addRenderPipelineFunctionsWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {
 auto *p=new NmbRenderPipelineDescriptor();
 const bool ok=renderDescriptor(d,p,error) && nmb_binary_archive_add_render(_handle,p);delete p;
 if(!ok)return failWith<BOOL>(error,@"archive render pipeline",NO);
 if(error)*error=nil;return YES;
}
- (BOOL)addComputePipelineFunctionsWithDescriptor:(MTLComputePipelineDescriptor *)d error:(NSError **)error {
 NmbComputePipelineDescriptor p={};if(!computeDescriptor(d,&p,error))return NO;
 if(!nmb_binary_archive_add_compute_descriptor(_handle,&p))return failWith<BOOL>(error,@"archive compute pipeline",NO);
 if(error)*error=nil;return YES;
}
- (BOOL)serializeToURL:(NSURL *)url error:(NSError **)error {
 if(!url.isFileURL){if(error)*error=nbError(@"a file URL is required");return NO;}
 if(!nmb_binary_archive_serialize(_handle,url.fileSystemRepresentation))return failWith<BOOL>(error,@"archive serialize",NO);
 if(error)*error=nil;return YES;
}
@end

static bool pipelineArchives(NSArray *list,std::vector<NmbBinaryArchive *> &archives,NSError **error) {
 for(id archive in list) {
  if(![archive isKindOfClass:[NBBinaryArchive class]]){if(error)*error=nbError(@"foreign binary archive");return false;}
  archives.push_back([(NBBinaryArchive *)archive handle]);
 }
 return true;
}

static uint64_t askAnswer(SEL cmd,NSUInteger arg);
@interface NBDeviceMethods : NSObject
@end
// dada: texture memory in our own layout (Apple's AGX layouts are not known): each slice's levels one after another, each level's rows of
// texels or compressed blocks packed (nmb_device_linear_bytes_per_row), images of a 3D level one after another. CoreUI fills a texture's
// memory through newTextureLayoutWithDescriptor:isHeapOrBufferBacked: (ASTC assets, sometimes without asking respondsToSelector:: g2k8's
// SpringBoard died of the unrecognized selector) and hands it to newTextureWithBytesNoCopy:, which reads it back in the same layout.
static bool nbBlock(NSUInteger format,NSUInteger &bw,NSUInteger &bh,NSUInteger &bb) {
 static const uint8_t astc[16][2]={{4,2},{4,4},{5,4},{5,5},{6,5},{6,6},{8,4},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}};
 bw=bh=1;bb=0;
 if(format>=185 && format<=200){bw=astc[format-185][0];bh=astc[format-185][1];bb=16;return true;}
 if(format>=203 && format<=218){bw=astc[format-203][0];bh=astc[format-203][1];bb=16;return true;}
 if(format>=221 && format<=236){bw=astc[format-221][0];bh=astc[format-221][1];bb=16;return true;}
 switch(format) {
  case 130:case 131:case 140:case 141:case 170:case 172:case 180:case 181:case 182:case 183:bw=bh=4;bb=8;return true;   // BC1, BC4, EAC R11, ETC2
  case 132:case 133:case 134:case 135:case 142:case 143:case 150:case 151:case 152:case 153:case 174:case 176:case 178:case 179:
   bw=bh=4;bb=16;return true;   // BC2/3/5/6H/7, EAC RG11, EAC RGBA8
  default:break;
 }
 if(format>=160 && format<=167)return false;   // PVRTC: not in rows of blocks
 bb=gDevice?nmb_device_linear_bytes_per_row(gDevice,format,1):0;
 return bb!=0;
}
struct NBLinearLayout {
 NSUInteger bw=1,bh=1,bb=0,width=0,height=0,depth=0,levels=1,slices=1,size=0;
 bool ok=false;
 bool init(MTLTextureDescriptor *d) {
  if(!nbBlock(d.pixelFormat,bw,bh,bb) || d.sampleCount>1)return false;
  width=d.width;height=d.height;depth=d.textureType==MTLTextureType3D?d.depth:1;levels=d.mipmapLevelCount?d.mipmapLevelCount:1;
  slices=(d.arrayLength?d.arrayLength:1)*(d.textureType==MTLTextureTypeCube||d.textureType==MTLTextureTypeCubeArray?6:1);
  size=offset(slices,0);ok=true;return true;
 }
 NSUInteger w(NSUInteger level) const {return std::max<NSUInteger>(1,width>>level);}
 NSUInteger h(NSUInteger level) const {return std::max<NSUInteger>(1,height>>level);}
 NSUInteger dz(NSUInteger level) const {return std::max<NSUInteger>(1,depth>>level);}
 NSUInteger rowBytes(NSUInteger level) const {return (w(level)+bw-1)/bw*bb;}
 NSUInteger rows(NSUInteger level) const {return (h(level)+bh-1)/bh;}
 NSUInteger imageBytes(NSUInteger level) const {return rowBytes(level)*rows(level);}
 NSUInteger levelBytes(NSUInteger level) const {return imageBytes(level)*dz(level);}
 NSUInteger offset(NSUInteger slice,NSUInteger level) const {
  NSUInteger perSlice=0;for(NSUInteger l=0;l<levels;l++)perSlice+=levelBytes(l);
  NSUInteger at=slice*perSlice;for(NSUInteger l=0;l<level;l++)at+=levelBytes(l);return at;
 }
 // Copies `region` of (slice, level) between texture memory and linear bytes; `toTexture` picks the direction.
 void copy(uint8_t *texture,uint8_t *linear,NSUInteger rowStride,NSUInteger imageStride,NSUInteger slice,NSUInteger level,MTLRegion r,bool toTexture) const {
  if(!ok || slice>=slices || level>=levels)return;
  const NSUInteger base=offset(slice,level),row=rowBytes(level),image=imageBytes(level);
  const NSUInteger x=r.origin.x/bw*bb,bytes=(r.size.width+bw-1)/bw*bb,y=r.origin.y/bh,count=(r.size.height+bh-1)/bh;
  if(x+bytes>row || y+count>rows(level) || r.origin.z+r.size.depth>dz(level))return;
  for(NSUInteger z=0;z<r.size.depth;z++)for(NSUInteger k=0;k<count;k++) {
   uint8_t *t=texture+base+(r.origin.z+z)*image+(y+k)*row+x,*l=linear+z*imageStride+k*rowStride;
   if(toTexture)memcpy(t,l,bytes);else memcpy(l,t,bytes);
  }
 }
};
struct NBWatermark {uint64_t value[2];};
@interface NBTextureLayout : NSObject
@end
@implementation NBTextureLayout {NBLinearLayout _layout;}
- (instancetype)initWithLayout:(const NBLinearLayout &)layout {if((self=[super init]))_layout=layout;return self;}
- (unsigned long long)size {return _layout.size;}
- (unsigned long long)alignment {return 256;}
- (MTLSize)copyGranularity {return MTLSizeMake(_layout.bw,_layout.bh,1);}
- (NBWatermark)watermark {return NBWatermark{{0,0}};}
- (void)initializeTextureMemory:(void *)memory {if(memory)memset(memory,0,_layout.size);}
- (void)finalizeTextureMemory:(void *)memory {(void)memory;}
- (void)copyFromLinearBytes:(const void *)bytes linearOffset:(unsigned long long)offset linearBytesPerRow:(unsigned long long)row
        linearBytesPerImage:(unsigned long long)image toTextureMemory:(void *)memory textureSlice:(unsigned long long)slice
        textureLevel:(unsigned long long)level textureRegion:(MTLRegion)region {
 if(bytes && memory)_layout.copy((uint8_t *)memory,(uint8_t *)bytes+offset,row,image,slice,level,region,true);
}
- (void)copyFromTextureMemory:(const void *)memory textureSlice:(unsigned long long)slice textureLevel:(unsigned long long)level
        textureRegion:(MTLRegion)region toLinearBytes:(void *)bytes linearOffset:(unsigned long long)offset
        linearBytesPerRow:(unsigned long long)row linearBytesPerImage:(unsigned long long)image {
 if(bytes && memory)_layout.copy((uint8_t *)memory,(uint8_t *)bytes+offset,row,image,slice,level,region,false);
}
@end

@implementation NBDeviceMethods
- (NSString *)name {return gName;}
- (uint64_t)registryID {return gRegistryID;}
- (NSUInteger)maxBufferLength {return nmb_device_max_buffer_length(gDevice);}
- (BOOL)hasUnifiedMemory {return YES;}
// Core Image's CIMetalDeviceGetLimits reads both (Apple's GPU devices answer them; _MTLDevice does not): without them every CIContext
// creation threw and iconservicesagent aborted while rasterizing symbol glyphs. The GPU has no memory of its own; it shares the system's.
- (uint64_t)dedicatedMemorySize {return 0;}
- (uint64_t)sharedMemorySize {return [NSProcessInfo processInfo].physicalMemory;}
- (BOOL)isLowPower {return NO;}
- (BOOL)isRemovable {return NO;}
- (BOOL)isHeadless {return NO;}
// supportsFamily: and the other queries the runtime answers are installed from it (installAnswers); supportsFamily: also answers the target
// hardware's families (the bridge's TARGET_FAMILIES, journaled with their waived rows as F lines). No legacy feature set is claimed: the
// base implementation reads Apple tables this device does not have.
- (BOOL)supportsFeatureSet:(MTLFeatureSet)set {(void)set;return NO;}
// dada (iOS 26): Metal's own texture-from-buffer path asks the device (Apple's AGX device answers it; _MTLDevice does not). A texel buffer
// starts where a linear texture may: the runtime's answer for that, at least 16.
- (NSUInteger)minimumTextureBufferAlignmentForPixelFormat:(MTLPixelFormat)format {
 return (NSUInteger)MAX(askAnswer(sel_registerName("minimumLinearTextureAlignmentForPixelFormat:"),(NSUInteger)format),(uint64_t)16);
}
// The original discovery (_MTLAddDevice) runs these four on every device it registers. The target's own versions fill the base class's limits
// and feature tables from a hardware profile (`featureProfile`, `indirectArgumentBufferCapabilities`, a table of Apple GPUs) that this device
// has not got: running them would claim an Apple GPU's capabilities. They are empty here, so a limit or feature the runtime measured is answered
// by this file's own answers (installAnswers) and any other stays the base class's neutral 0/NO. Nothing is claimed that nothing measured.
- (void)initLimits {}
- (void)initFeatureQueries {}
- (void)initGPUFamilySupport {}
- (void)initWorkarounds {}
- (NSString *)productName {return gName;}
- (NSString *)vendorName {char name[64]={};nmb_device_vendor_name(gDevice,name,sizeof name);return [NSString stringWithUTF8String:name];}
- (MTLSize)maxThreadsPerThreadgroup {
 uint64_t s[3]={};if(!nmb_device_max_threads_per_threadgroup(gDevice,s))NBLOG(@"native-metal: maxThreadsPerThreadgroup: %@",lastError());
 return MTLSizeMake(s[0],s[1],s[2]);
}
- (void)doesNotRecognizeSelector:(SEL)selector {
 if(gMissing)gMissing("NativeBridgeMetalDevice",sel_getName(selector));
 noteProblem([NSString stringWithFormat:@"missing device %s",sel_getName(selector)]);
 [super doesNotRecognizeSelector:selector];
}
- (id<MTLEvent>)newEvent {
 NmbEvent *handle=nmb_event_new(gDevice,false);if(!handle)return nil;
 NBEvent *event=[NBEvent new];event.handle=handle;return (id<MTLEvent>)event;
}
- (id<MTLSharedEvent>)newSharedEvent {
 NmbEvent *handle=nmb_event_new(gDevice,true);if(!handle)return nil;
 NBSharedEvent *event=[NBSharedEvent new];event.handle=handle;return (id<MTLSharedEvent>)event;
}
- (id<MTLFence>)newFence {
 NmbFence *handle=nmb_fence_new(gDevice);if(!handle)return nil;
 NBFence *fence=[NBFence new];fence.handle=handle;return (id<MTLFence>)fence;
}
- (id<MTLCommandQueue>)newCommandQueue {
 NmbQueue *handle=nmb_queue_new(gDevice);if(!handle)return nil;
 NBQueue *queue=[NBQueue new];queue.handle=handle;return (id<MTLCommandQueue>)queue;
}
// dada: neutral defaults (0) before: with nothing allocated and no working set, CoreAnimation's caches in backboardd grew past a gigabyte of
// screen-sized textures and the phone stalled (g2k15). The working set: 2 GB a process, of the GPU's 15.5 GB of addresses shared by all.
- (NSUInteger)currentAllocatedSize {return (NSUInteger)nmb_device_current_allocated_size(gDevice);}
// RAM-only iOS root consumes most of the boot window; keep CA's cache advice
// within the available userland budget. This is a recommendation, not an allocation cap.
- (uint64_t)recommendedMaxWorkingSetSize {return !strcmp(getprogname(),"backboardd") ? 512ull<<20 : 256ull<<20;}
// dada: answered 0 by the neutral defaults before (NBDefaults.h), so CoreAnimation sized its heaps for nothing and the textures it then
// placed in them were refused (OutOfMemory(Heap), 300 in g2k10: the lock screen's flicker).
- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)d {
 MTLSizeAndAlign answer={0,1};uint64_t out[2]={0,1};NmbTextureDescriptor descriptor;
 if(d && textureDescriptor(d,&descriptor) && nmb_device_heap_texture_size_and_align(gDevice,&descriptor,out)){answer.size=out[0];answer.align=out[1];}
 else NBLOG(@"native-metal: heapTextureSizeAndAlignWithDescriptor: refused: %@",lastError());
 return answer;
}
- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
 MTLSizeAndAlign answer={length,256};uint64_t out[2];
 if(nmb_device_heap_buffer_size_and_align(gDevice,length,(uint64_t)options,out)){answer.size=out[0];answer.align=out[1];}
 return answer;
}
- (id<MTLHeap>)newHeapWithDescriptor:(MTLHeapDescriptor *)d {
 if(!d)return nil;
 NmbHeap *handle=nmb_heap_new(gDevice,d.type,d.size,(uint64_t)d.resourceOptions);
 if(!handle){NBLOG(@"native-metal: heap refused: %@",lastError());return nil;}
 NBHeap *heap=[NBHeap new];heap.handle=handle;heap.options=d.resourceOptions;heap.heapType=d.type;return (id<MTLHeap>)heap;
}
- (id)newResourceGroupFromResources:(const id __unsafe_unretained [])resources count:(NSUInteger)count {
 NBResourceGroup *group=[NBResourceGroup new];
 NSMutableArray *held=[NSMutableArray arrayWithCapacity:count];
 for(NSUInteger i=0;i<count;i++)if(resources[i])[held addObject:resources[i]];
 group.resources=held;return group;
}
- (id<MTLTexture>)newTextureWithBytesNoCopy:(void *)bytes length:(NSUInteger)length descriptor:(MTLTextureDescriptor *)d deallocator:(void (^)(void *,NSUInteger))deallocator {
 // data-a-live: a one-level 2D texture in rows of texels or blocks (bb25: QuartzCore in backboardd asks for the App
 // Library's full-screen ASTC 4x4 image, shader-read only, 282 x 609 blocks of 16 bytes). Made as an ordinary texture
 // and filled from the bytes once; the caller's deallocator runs when the texture goes. A read-only texture sees no
 // later writes to the bytes, which a texture over them would.
 static const uint8_t astc[16][2]={{4,2},{4,4},{5,4},{5,5},{6,5},{6,6},{8,4},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}};
 const NSUInteger format=d.pixelFormat;
 NSUInteger bw=1,bh=1,bb=0;
 if(format>=185 && format<=200){bw=astc[format-185][0];bh=astc[format-185][1];bb=16;}
 else if(format>=203 && format<=218){bw=astc[format-203][0];bh=astc[format-203][1];bb=16;}
 else switch(format){case 1:case 10:bb=1;break;case 30:bb=2;break;case 70:case 71:case 80:case 81:bb=4;break;case 115:bb=8;break;default:break;}
 const NSUInteger rowBytes=(d.width+bw-1)/bw*bb,rows=(d.height+bh-1)/bh;
 // dada: RenderBox hands its ASTC images (CGImageTextureData, the Calendar and Clock icons of g2k9) with a length of 0: the memory is the
 // texture's whole layout, as Metal takes it without a length check.
 if(bytes && !length){NBLinearLayout whole;if(whole.init(d))length=whole.size;}
 // dada: more than one level or slice: the memory is in our layout (newTextureLayoutWithDescriptor:), copied level by level once
 {NBLinearLayout layout;
  if(bytes && (d.mipmapLevelCount>1 || d.arrayLength>1 || d.textureType!=MTLTextureType2D) && layout.init(d) && length>=layout.size) {
   id<MTLTexture> texture=[(id<MTLDevice>)self newTextureWithDescriptor:d];
   if(texture) {
    for(NSUInteger slice=0;slice<layout.slices;slice++)for(NSUInteger level=0;level<layout.levels;level++)
     [texture replaceRegion:MTLRegionMake3D(0,0,0,layout.w(level),layout.h(level),layout.dz(level)) mipmapLevel:level slice:slice
                  withBytes:(const uint8_t *)bytes+layout.offset(slice,level) bytesPerRow:layout.rowBytes(level) bytesPerImage:layout.imageBytes(level)];
    if(deallocator) {
     static char layoutOwnerKey;
     NSData *owner=[[NSData alloc] initWithBytesNoCopy:bytes length:length deallocator:^(void *b,NSUInteger l){deallocator(b,l);}];
     objc_setAssociatedObject(texture,&layoutOwnerKey,owner,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return texture;
   }
  }}
 if(bytes && bb && d.textureType==MTLTextureType2D && d.mipmapLevelCount==1 && d.arrayLength==1 && d.depth==1 &&
    d.sampleCount==1 && length>=rowBytes*rows) {
  id<MTLTexture> texture=[(id<MTLDevice>)self newTextureWithDescriptor:d];
  if(texture) {
   const int probeLevel=pandoraAstcProbeLevel();const bool probe=probeLevel>=1;
   if(probe && bb==16 && d.width>=512)   // data-a-live: QuartzCore's own descriptor of the image (bb39)
    NSLog(@"pandora-metal astc descriptor: usage %lu storage %lu cpu cache %lu hazard %lu resource options %lu swizzle %u/%u/%u/%u "
          "type %lu mips %lu samples %lu allow gpu opt %d | bytes %p length %lu",(unsigned long)d.usage,(unsigned long)d.storageMode,
          (unsigned long)d.cpuCacheMode,(unsigned long)d.hazardTrackingMode,(unsigned long)d.resourceOptions,(unsigned)d.swizzle.red,
          (unsigned)d.swizzle.green,(unsigned)d.swizzle.blue,(unsigned)d.swizzle.alpha,(unsigned long)d.textureType,
          (unsigned long)d.mipmapLevelCount,(unsigned long)d.sampleCount,(int)d.allowGPUOptimizedContents,
          bytes,(unsigned long)length);
   if(probe && bb==16 && d.width>=512) {
    std::vector<uint8_t> copy((const uint8_t *)bytes,(const uint8_t *)bytes+rowBytes*rows);
    static const uint8_t red[16]={0xfc,0xfd,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff};
    const NSUInteger blocks=rowBytes/16;
    for(NSUInteger y=0;y<rows;y++)for(NSUInteger x=0;x<blocks/2;x++)memcpy(&copy[y*rowBytes+x*16],red,16);
    static std::atomic<int> handed{0};
    const int handedNth=handed.fetch_add(1);
    // Level 2: every large ASTC image is handed over as BGRA8; level 3: every other one (the first BGRA8, the second
    // ASTC with its left half red), so one run shows both.
    if(probeLevel==2 || (probeLevel>=3 && handedNth%2==0)) {
     // bb35: the ASTC blocks are in the texture exactly as uploaded, yet no red is drawn. Level 2 hands QuartzCore a
     // BGRA8 texture of the same size instead (left half red, right half green): colour on screen = only ASTC
     // sampling is wrong; still transparent = the sampling or the draw is wrong whatever the format.
     MTLTextureDescriptor *plain=[d copy];plain.pixelFormat=MTLPixelFormatBGRA8Unorm;
     id<MTLTexture> swap=[(id<MTLDevice>)self newTextureWithDescriptor:plain];
     if(swap) {
      std::vector<uint32_t> px((size_t)d.width*d.height);
      for(NSUInteger y=0;y<d.height;y++)for(NSUInteger x=0;x<d.width;x++)px[y*d.width+x]=x<d.width/2?0xffff0000u:0xff00ff00u;
      [swap replaceRegion:MTLRegionMake2D(0,0,d.width,d.height) mipmapLevel:0 withBytes:px.data() bytesPerRow:d.width*4];
      texture=swap;
      objc_setAssociatedObject(swap,&gProbeReadbackKey,@YES,OBJC_ASSOCIATION_RETAIN_NONATOMIC);   // read back its targets too
      NSLog(@"pandora-metal astc probe: %lux%lu handed over as BGRA8, left red right green",(unsigned long)d.width,(unsigned long)d.height);
     }
    } else {
    // bb34: no red reached any render target. The first image is uploaded by replaceRegion, the second through a
    // shared buffer and a blit; each is then copied back by the GPU and compared block by block with what went up.
    static std::atomic<int> nth{0};
    const int which=nth.fetch_add(1);
    static id<MTLCommandQueue> queue=[(id<MTLDevice>)self newCommandQueue];
    const NSUInteger size=rowBytes*rows;
    if(which%2==0)
     [texture replaceRegion:MTLRegionMake2D(0,0,d.width,d.height) mipmapLevel:0 withBytes:copy.data() bytesPerRow:rowBytes];
    else {
     id<MTLBuffer> up=[(id<MTLDevice>)self newBufferWithBytes:copy.data() length:size options:MTLResourceStorageModeShared];
     id<MTLCommandBuffer> c=[queue commandBuffer];id<MTLBlitCommandEncoder> b=[c blitCommandEncoder];
     [b copyFromBuffer:up sourceOffset:0 sourceBytesPerRow:rowBytes sourceBytesPerImage:size sourceSize:MTLSizeMake(d.width,d.height,1)
             toTexture:texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
     [b endEncoding];[c commit];[c waitUntilCompleted];
    }
    id<MTLBuffer> back=[(id<MTLDevice>)self newBufferWithLength:size options:MTLResourceStorageModeShared];
    memset(back.contents,0xa5,size);
    id<MTLCommandBuffer> c=[queue commandBuffer];id<MTLBlitCommandEncoder> b=[c blitCommandEncoder];
    [b copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(d.width,d.height,1)
              toBuffer:back destinationOffset:0 destinationBytesPerRow:rowBytes destinationBytesPerImage:size];
    [b endEncoding];[c commit];[c waitUntilCompleted];
    unsigned same[2]={},zero[2]={},untouched[2]={},other[2]={};
    const uint8_t *got=(const uint8_t *)back.contents;
    for(NSUInteger y=0;y<rows;y++)for(NSUInteger x=0;x<blocks;x++) {
     const uint8_t *g=got+y*rowBytes+x*16,*w=&copy[y*rowBytes+x*16];const int half=x<blocks/2?0:1;
     bool z=true,u=true;for(int k=0;k<16;k++){if(g[k])z=false;if(g[k]!=0xa5)u=false;}
     if(!memcmp(g,w,16))same[half]++;else if(z)zero[half]++;else if(u)untouched[half]++;else other[half]++;
    }
    NSLog(@"pandora-metal astc probe #%d %lux%lu by %s, left %lu of %lu block columns red; read back left same/zero/untouched/other "
          "%u/%u/%u/%u right %u/%u/%u/%u (status %ld)",which,(unsigned long)d.width,(unsigned long)d.height,which%2==0?"replaceRegion":"blit",
          (unsigned long)(blocks/2),(unsigned long)blocks,same[0],zero[0],untouched[0],other[0],same[1],zero[1],untouched[1],other[1],(long)c.status);
    }
   } else
   [texture replaceRegion:MTLRegionMake2D(0,0,d.width,d.height) mipmapLevel:0 withBytes:bytes bytesPerRow:rowBytes];
   if(deallocator) {
    static char ownerKey;
    NSData *owner=[[NSData alloc] initWithBytesNoCopy:bytes length:length deallocator:^(void *b,NSUInteger l){deallocator(b,l);}];
    objc_setAssociatedObject(texture,&ownerKey,owner,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
   }
   static std::atomic<uint32_t> made{0};
   if(bb==16) {   // data-a-live: the block layout of the bytes, 8x8 grid
    static std::atomic<int> described{0};
    if(described.fetch_add(1)<6) {
     const uint8_t *blocks=(const uint8_t *)bytes;
     unsigned zero[8][8]={},single[8][8]={},other[8][8]={};
     for(NSUInteger y=0;y<rows;y++)for(NSUInteger x=0;x<rowBytes/16;x++) {
      const uint8_t *b=blocks+y*rowBytes+x*16;
      bool z=true;for(unsigned k=0;k<16;k++)if(b[k]){z=false;break;}
      const unsigned gy=(unsigned)(y*8/rows),gx=(unsigned)(x*8/(rowBytes/16));
      if(z)zero[gy][gx]++;else if(((b[0]|(b[1]<<8))&0x1ff)==0x1fc)single[gy][gx]++;else other[gy][gx]++;
     }
     void *frames[8];const int n=backtrace(frames,8);std::string callers;char item[96];
     for(int i=1;i<n;i++){Dl_info info{};if(!dladdr(frames[i],&info) || !info.dli_fname)continue;const char *slash=strrchr(info.dli_fname,'/');
      snprintf(item,sizeof item," %s+%lx",slash?slash+1:info.dli_fname,(unsigned long)((uintptr_t)frames[i]-(uintptr_t)info.dli_fbase));callers+=item;}
     const unsigned image=(unsigned)described.load();
     NSLog(@"pandora-metal nocopy blocks #%u %lux%lu %lu per row |%s",image,(unsigned long)d.width,(unsigned long)d.height,(unsigned long)rowBytes,callers.c_str());
     for(unsigned gy=0;gy<8;gy++) {
      NSMutableString *line=[NSMutableString string];
      for(unsigned gx=0;gx<8;gx++) {
       const unsigned all=zero[gy][gx]+single[gy][gx]+other[gy][gx];
       [line appendFormat:@" %02u/%02u/%02u",all?zero[gy][gx]*99/all:0,all?single[gy][gx]*99/all:0,all?other[gy][gx]*99/all:0];
      }
      NSLog(@"pandora-metal nocopy grid #%u r%u z/v/o:%@",image,gy,line);
     }
    }
   }
   static std::atomic<int> captures{0};
   if(captures.fetch_add(1)<2)gCapture.store(800);
   if(++made<=5)NSLog(@"pandora-metal nocopy: made fmt %lu %lux%lu, %lu bytes per row, copied once",(unsigned long)format,
                      (unsigned long)d.width,(unsigned long)d.height,(unsigned long)rowBytes);
   return texture;
  }
 }
 NBLOG(@"native-metal: newTextureWithBytesNoCopy:length:descriptor:deallocator: is not implemented (Apple's texture layout of raw memory is not known)");
 // data-a-live: what is asked for and still refused: the descriptor, the memory and the callers (image + offset), the
 // first 20 calls and then one in 100.
 static std::atomic<uint32_t> asked{0};
 const uint32_t nth=++asked;
 if(nth<=20 || nth%100==0) {
  void *frames[10];const int n=backtrace(frames,10);
  std::string callers;char item[96];
  for(int i=1;i<n;i++) {
   Dl_info info{};
   if(!dladdr(frames[i],&info) || !info.dli_fname)continue;
   const char *slash=strrchr(info.dli_fname,'/');
   snprintf(item,sizeof item," %s+%lx",slash?slash+1:info.dli_fname,(unsigned long)((uintptr_t)frames[i]-(uintptr_t)info.dli_fbase));
   callers+=item;
  }
  NSLog(@"pandora-metal nocopy #%u: fmt %lu type %lu %lux%lux%lu mips %lu array %lu usage %lx storage %lu opts %lx"
        " bytes %p (page +%lx) length %lu (per row if one plane: %lu) |%s",nth,(unsigned long)d.pixelFormat,
        (unsigned long)d.textureType,(unsigned long)d.width,(unsigned long)d.height,(unsigned long)d.depth,
        (unsigned long)d.mipmapLevelCount,(unsigned long)d.arrayLength,(unsigned long)d.usage,(unsigned long)d.storageMode,
        (unsigned long)d.resourceOptions,bytes,(unsigned long)((uintptr_t)bytes&0x3fff),(unsigned long)length,
        (unsigned long)(d.height?length/d.height:0),callers.c_str());
 }
 return nil;
}
// dada: our layout (NBLinearLayout above). Answered nil, CoreUI made a zero-length "texture memory" of an ASTC asset that CoreGraphics
// then read past (SpringBoard's SIGBUS in CGImageTextureDataGetOffsetWithBlock, every respring of g2k7).
- (id)newTextureLayoutWithDescriptor:(MTLTextureDescriptor *)d isHeapOrBufferBacked:(BOOL)backed {
 (void)backed;
 NBLinearLayout layout;
 if(!d || !layout.init(d)){NBLOG(@"native-metal: no texture layout of pixel format %lu",(unsigned long)d.pixelFormat);return nil;}
 return [[NBTextureLayout alloc] initWithLayout:layout];
}
- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)count {(void)count;return [(id<MTLDevice>)self newCommandQueue];}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
 NmbBuffer *handle=nmb_buffer_new(gDevice,length,options,nullptr);
 if(!handle){NBLOG(@"native-metal: buffer refused: %@",lastError());return nil;}
 NBBuffer *buffer=[NBBuffer new];buffer.handle=handle;buffer.options=options;return (id<MTLBuffer>)buffer;
}
- (id<MTLBuffer>)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options {
 if(!bytes)return nil;
 NmbBuffer *handle=nmb_buffer_new(gDevice,length,options,bytes);
 if(!handle){NBLOG(@"native-metal: buffer refused: %@",lastError());return nil;}
 NBBuffer *buffer=[NBBuffer new];buffer.handle=handle;buffer.options=options;return (id<MTLBuffer>)buffer;
}
- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)pointer length:(NSUInteger)length options:(MTLResourceOptions)options deallocator:(void (^)(void *,NSUInteger))deallocator {
 void *context=deallocator?(__bridge_retained void *)[deallocator copy]:nullptr;
 NmbBuffer *handle=nmb_buffer_new_no_copy_releasing(gDevice,pointer,length,options,context?releaseBlock:nullptr,context);
 if(!handle) {
  if(context)(void)(__bridge_transfer id)context;
  // dada: the kernel imports IOSurfaces only; other memory is mirrored (NBBuffer.mirror) instead of refused (g2vk13: QuartzCore's
  // no-copy buffers, 358 of them, were refused and their layers were not drawn).
  NmbBuffer *gpu=nmb_buffer_new(gDevice,length,options,pointer);
  if(!gpu){NBLOG(@"native-metal: no-copy buffer refused, and no buffer to mirror it: %@",lastError());return nil;}
  static std::atomic<unsigned> said;if(said++<3)consoleLine([NSString stringWithFormat:@"no-copy buffer of %lu bytes mirrored",(unsigned long)length]);
  NBBuffer *buffer=[NBBuffer new];buffer.handle=gpu;buffer.options=options;buffer.wrapped=pointer;buffer.mirror=pointer;buffer.mirrorLength=length;buffer.mirrorRelease=deallocator;
  {std::lock_guard<std::mutex> lock(gMirrorLock);if(!gMirrors)gMirrors=[NSHashTable weakObjectsHashTable];[gMirrors addObject:buffer];}
  return (id<MTLBuffer>)buffer;
 }
 gNoCopyAll++;if(context)gNoCopyMade++;
 NBBuffer *buffer=[NBBuffer new];buffer.handle=handle;buffer.options=options;buffer.wrapped=pointer;
 return (id<MTLBuffer>)buffer;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d {
 if(capturing() && d.width<=64 && d.height<=8) {   // dada (g2k65): who makes the wallpaper's 64x8 gradients, in which format
  NSArray *stack=[NSThread callStackSymbols];NSMutableString *o=[NSMutableString string];
  for(NSUInteger i=1;i<stack.count && i<14;i++)[o appendFormat:@" | %@",[stack[i] substringFromIndex:MIN((NSUInteger)40,[stack[i] length])]];
  capture([NSString stringWithFormat:@"newtex %lux%lu pf %lu usage %lu%@",(unsigned long)d.width,(unsigned long)d.height,(unsigned long)d.pixelFormat,(unsigned long)d.usage,o]);
 }
 NmbTextureDescriptor descriptor;if(!textureDescriptor(d,&descriptor))return nil;
 NmbTexture *handle=nmb_texture_new(gDevice,&descriptor);noteFormat((uint32_t)descriptor.format,handle!=nullptr);
 if(!handle){NBLOG(@"native-metal: texture refused: %@",lastError());return nil;}
 NBTexture *texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;return (id<MTLTexture>)texture;
}
// The surface's pages are the texels: its mapping in this process is imported without a copy (pinned by the kernel, mapped into the GPU's
// 4 KiB context by Turnip), so every process that imports the surface renders and samples the same memory.
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)surface plane:(NSUInteger)plane {
 // A planar surface (Apple's lossless-compressed BGRA spelling has one plane, video formats two) is imported whole; the texture starts at
 // the plane's own address and rows. Only a plain surface has no planes, and then only plane 0 exists.
 const size_t planes=surface?IOSurfaceGetPlaneCount(surface):0;
 if(!surface || !d || (planes?plane>=planes:plane!=0)){refuseSurface(1,surface,d,plane,0,0);return nil;}
 if(planes)gSurfacePlanar++;
 {static std::atomic<int> told{0};   // dada: which surfaces a process takes in, screen-sized ones (the wallpaper's, g2k19)
  if(IOSurfaceGetWidth(surface)*IOSurfaceGetHeight(surface)>=1000000 && told.fetch_add(1)<60) {
   const uint32_t *px=nullptr;uint32_t mid=0;
   if(IOSurfaceLock(surface,kIOSurfaceLockReadOnly,nullptr)==0){px=(const uint32_t *)IOSurfaceGetBaseAddress(surface);
    if(px)mid=px[(IOSurfaceGetHeight(surface)/2)*(IOSurfaceGetBytesPerRow(surface)/4)+IOSurfaceGetWidth(surface)/2];IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,nullptr);}
   consoleLine([NSString stringWithFormat:@"surface taken in: %u %zux%zu pf %lu, the middle %08x now",IOSurfaceGetID(surface),IOSurfaceGetWidth(surface),
    IOSurfaceGetHeight(surface),(unsigned long)d.pixelFormat,mid]);
  }}
 void *base=IOSurfaceGetBaseAddress(surface);const size_t page=(size_t)getpagesize(),allocation=IOSurfaceGetAllocSize(surface);
 if(!page || (page&(page-1)) || !base || ((uintptr_t)base&(page-1)) || !allocation || allocation>SIZE_MAX-(page-1)){refuseSurface(2,surface,d,plane,(uintptr_t)base,0);return nil;}
 uintptr_t start=planes?(uintptr_t)IOSurfaceGetBaseAddressOfPlane(surface,plane):(uintptr_t)base;
 size_t row=planes?IOSurfaceGetBytesPerRowOfPlane(surface,plane):IOSurfaceGetBytesPerRow(surface);
 size_t computed[4];   // data-a-live: a 'b3a8' surface without plane rows (bb43)
 if(!row && plane<2 && pandoraB3a8Planes(surface,allocation,computed)){start=(uintptr_t)base+computed[plane*2];row=computed[plane*2+1];}
 if(start<(uintptr_t)base || start-(uintptr_t)base>=allocation || !row){refuseSurface(3,surface,d,plane,start-(uintptr_t)base,row);return nil;}
 const size_t bytes=(allocation+page-1)&~(page-1);
 NBBuffer *buffer=surfaceBuffer(surface,base,bytes);if(!buffer){refuseSurface(4,surface,d,plane,start-(uintptr_t)base,row);return nil;}
 NBTexture *texture=nil;
 // dada: RenderBox's RGB10A8_2P_XR10 textures over surfaces our IOSurface reports as one 'BGRA' plane although they hold both:
 // the RGB10 rows, then the alpha bytes at w a row (g2k16: 256x256, 1024 a row, 327680 bytes; refused for want of plane 1, the
 // icons' backgrounds and the clock's face missing). Plane 1 as an Apple 'b3a8' surface has it, counted here.
 if(planes<2 && plane==0 && (d.pixelFormat==550 || d.pixelFormat==551)) {
  const size_t w=IOSurfaceGetWidth(surface),h=IOSurfaceGetHeight(surface),offset=start-(uintptr_t)base;
  const size_t alpha=offset+row*h,alphaRow=h && allocation>alpha?(allocation-alpha)/h:0;
  NmbTextureDescriptor descriptor;
  if(w && alphaRow>=w && textureDescriptor(d,&descriptor)) {
   NmbTexture *handle=nmb_texture_new_on_surface(buffer.handle,&descriptor,offset,row,0x62336138 /* 'b3a8' */,0,w,h,alpha,alphaRow);
   if(handle){texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;
    texture.buffer=buffer;texture.offset=offset;texture.rowBytes=row;}
   else NBLOG(@"native-metal: RGB10A8_2P_XR10 over a one-plane surface refused: %@",lastError());
  }
 }
 if(!texture)texture=planes>=2?textureOnSurface(buffer,d,surface,plane,start-(uintptr_t)base,row):textureOnBuffer(buffer,d,start-(uintptr_t)base,row,true);
 if(!texture){refuseSurface(5,surface,d,plane,start-(uintptr_t)base,row);return nil;}
 gSurfaceTextures++;texture.surface=(IOSurfaceRef)CFRetain(surface);return (id<MTLTexture>)texture;
}
- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error {
 const void *bytes=nullptr;size_t size=0;dispatch_data_t mapped=dispatch_data_create_map(data,&bytes,&size);
 const uint64_t libraryBegan=nowUs();
 NmbLibrary *handle=nmb_library_new(gDevice,bytes,size,false);(void)mapped;gTimeLibrary.add(libraryBegan);
 if(!handle)return failWith<id<MTLLibrary>>(error,@"library",nil);
 NBLibrary *library=[NBLibrary new];library.handle=handle;if(error)*error=nil;return (id<MTLLibrary>)library;
}
- (id<MTLLibrary>)newLibraryWithFile:(NSString *)path error:(NSError **)error {
 NSData *bytes=path?[NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:error]:nil;if(!bytes)return nil;
 const uint64_t libraryBegan=nowUs();
 NmbLibrary *handle=nmb_library_new(gDevice,bytes.bytes,bytes.length,false);gTimeLibrary.add(libraryBegan);
 if(!handle)return failWith<id<MTLLibrary>>(error,@"library",nil);
 NBLibrary *library=[NBLibrary new];library.handle=handle;if(error)*error=nil;return (id<MTLLibrary>)library;
}
- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error {
 if(!url.isFileURL){if(error)*error=nbError(@"a file URL is required");return nil;}
 return [(id<MTLDevice>)self newLibraryWithFile:url.path error:error];
}
- (id<MTLLibrary>)newDefaultLibraryWithBundle:(NSBundle *)bundle error:(NSError **)error {
 NSURL *url=[bundle URLForResource:@"default" withExtension:@"metallib"];
 if(!url){if(error)*error=nbError(@"the bundle has no default.metallib");return nil;}
 return [(id<MTLDevice>)self newLibraryWithURL:url error:error];
}
- (id<MTLLibrary>)newDefaultLibrary {return [(id<MTLDevice>)self newDefaultLibraryWithBundle:[NSBundle mainBundle] error:nil];}
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function error:(NSError **)error {
 return [(id<MTLDevice>)self newComputePipelineStateWithFunction:function options:MTLPipelineOptionNone reflection:nil error:error];
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function options:(MTLPipelineOption)options
                                                         reflection:(MTLAutoreleasedComputePipelineReflection *)reflection error:(NSError **)error {
 if(reflection)*reflection=nil;
 if(![(id)function isKindOfClass:[NBFunction class]]){if(error)*error=nbError(@"foreign function");return nil;}
 const NmbPipelineOptions creation={(uint64_t)options,nullptr,0};
 const uint64_t pipelineBegan=nowUs();
 NmbComputePipeline *handle=nmb_compute_pipeline_new_with_options(gDevice,[(NBFunction *)function handle],&creation);gTimePipeline.add(pipelineBegan);
 if(!handle)return failWith<id<MTLComputePipelineState>>(error,[NSString stringWithFormat:@"compute pipeline %@ {%s}",function.name,
  ((NBFunction *)function).constantsText.c_str()],nil);
 NBComputePipeline *state=[NBComputePipeline new];state.handle=handle;gPipelines++;if(error)*error=nil;return (id<MTLComputePipelineState>)state;
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d options:(MTLPipelineOption)options
                                                       reflection:(MTLAutoreleasedComputePipelineReflection *)reflection error:(NSError **)error {
 if(reflection)*reflection=nil;
 NmbComputePipelineDescriptor p={};
 if(!computeDescriptor(d,&p,error)){NSLog(@"gpu2-metal: compute pipeline (descriptor) %@ refused: %@",d.computeFunction.name,error?*error:nil);return nil;}
 std::vector<NmbBinaryArchive *> archives;
 if(!pipelineArchives(d.binaryArchives,archives,error))return nil;
 const NmbPipelineOptions creation={(uint64_t)options,archives.data(),archives.size()};
 const uint64_t pipelineBegan=nowUs();
 NmbComputePipeline *handle=nmb_compute_pipeline_new_with_descriptor(gDevice,&p,&creation);gTimePipeline.add(pipelineBegan);
 if(!handle)return failWith<id<MTLComputePipelineState>>(error,[NSString stringWithFormat:@"compute pipeline (descriptor) %@ %@",d.computeFunction.name,d.label],nil);
 NBComputePipeline *state=[NBComputePipeline new];state.handle=handle;state.label=d.label;gPipelines++;
 if(error)*error=nil;return (id<MTLComputePipelineState>)state;
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {
 return [(id<MTLDevice>)self newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:error];
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d options:(MTLPipelineOption)options
                                                        reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection error:(NSError **)error {
 if(reflection)*reflection=nil;
 auto *p=new NmbRenderPipelineDescriptor();
 if(!renderDescriptor(d,p,error)){delete p;return nil;}
 std::vector<NmbBinaryArchive *> archives;
 if(!pipelineArchives(d.binaryArchives,archives,error)){delete p;return nil;}
 const NmbPipelineOptions creation={(uint64_t)options,archives.data(),archives.size()};
 const uint64_t pipelineBegan=nowUs();const NBCost costBegan=NBCost::now();
 NmbRenderPipeline *handle=nmb_render_pipeline_new_with_options(gDevice,p,&creation);delete p;gTimePipeline.add(pipelineBegan);
 journalRenderPipeline(d,pipelineBegan,handle?nullptr:nmb_last_error(),gJournalPath.empty()?std::string():NBCost::now().since(costBegan));
 if(!handle) {
  // dada: what the descriptor links, for a pipeline that needs a linked function (RenderBox's custom effects)
  static std::atomic<unsigned> said;
  if(strstr(nmb_last_error(),"LinkedFunction") && said++<4) {
   MTLLinkedFunctions *l=d.fragmentLinkedFunctions;
   NSMutableString *t=[NSMutableString stringWithFormat:@"links of %@: functions %lu privateFunctions %lu binaryFunctions %lu groups %@",
    d.fragmentFunction.name,(unsigned long)l.functions.count,(unsigned long)l.privateFunctions.count,(unsigned long)l.binaryFunctions.count,l.groups.allKeys];
   for(id<MTLFunction> f in l.functions)[t appendFormat:@" f:%@(%@)",f.name,NSStringFromClass([(id)f class])];
   for(id<MTLFunction> f in l.privateFunctions)[t appendFormat:@" p:%@(%@)",f.name,NSStringFromClass([(id)f class])];
   for(id<MTLFunction> f in l.binaryFunctions)[t appendFormat:@" b:%@(%@)",f.name,NSStringFromClass([(id)f class])];
   if([d respondsToSelector:@selector(fragmentPreloadedLibraries)])[t appendFormat:@" preloaded %lu",(unsigned long)d.fragmentPreloadedLibraries.count];
   [t appendFormat:@" | fragment's library %@",NSStringFromClass([(id)d.fragmentFunction class])];
   consoleLine(t);
  }
  {   // dada: which constants each function was specialized with (WeatherPoster's pipelines need a constant's value, g2k22)
   auto consts=[](id<MTLFunction> f)->std::string{return [(id)f isKindOfClass:[NBFunction class]]?((NBFunction *)f).constantsText:std::string("?");};
   return failWith<id<MTLRenderPipelineState>>(error,[NSString stringWithFormat:@"render pipeline %@ {%s} / %@ {%s}",d.vertexFunction.name,
    consts(d.vertexFunction).c_str(),d.fragmentFunction.name,consts(d.fragmentFunction).c_str()],nil);
  }
 }
 NBRenderPipeline *state=[NBRenderPipeline new];state.handle=handle;gPipelines++;if(error)*error=nil;
 objc_setAssociatedObject(state,&gCapNamesKey,[NSString stringWithFormat:@"%@ / %@ c0 f%lu blend %d",d.vertexFunction.name,d.fragmentFunction.name,
  (unsigned long)d.colorAttachments[0].pixelFormat,(int)d.colorAttachments[0].blendingEnabled],OBJC_ASSOCIATION_RETAIN_NONATOMIC);
 if(pandoraAstcProbe()) {   // data-a-live: the whole pipeline, for the probe's draw log
  NBFunction *vf=[(id)d.vertexFunction isKindOfClass:[NBFunction class]]?(NBFunction *)d.vertexFunction:nil;
  NBFunction *ff=[(id)d.fragmentFunction isKindOfClass:[NBFunction class]]?(NBFunction *)d.fragmentFunction:nil;
  MTLRenderPipelineColorAttachmentDescriptor *a=d.colorAttachments[0];
  NSMutableString *full=[NSMutableString stringWithFormat:@"v %@ {%s} f %@ {%s} c0 %lu mask %lu blend %d %lu/%lu/%lu %lu/%lu/%lu c1 %lu mask %lu blend %d samples %lu",
   d.vertexFunction.name,vf?vf.constantsText.c_str():"",d.fragmentFunction.name,ff?ff.constantsText.c_str():"",(unsigned long)a.pixelFormat,
   (unsigned long)a.writeMask,(int)a.blendingEnabled,(unsigned long)a.sourceRGBBlendFactor,(unsigned long)a.destinationRGBBlendFactor,
   (unsigned long)a.rgbBlendOperation,(unsigned long)a.sourceAlphaBlendFactor,(unsigned long)a.destinationAlphaBlendFactor,
   (unsigned long)a.alphaBlendOperation,(unsigned long)d.colorAttachments[1].pixelFormat,(unsigned long)d.colorAttachments[1].writeMask,
   (int)d.colorAttachments[1].blendingEnabled,(unsigned long)d.rasterSampleCount];
  if(MTLVertexDescriptor *v=d.vertexDescriptor)for(unsigned i=0;i<31;i++) {
   MTLVertexAttributeDescriptor *at=v.attributes[i];if(at.format==MTLVertexFormatInvalid)continue;
   [full appendFormat:@" a%u=%lu:%lu:%lu:%lu",i,(unsigned long)at.format,(unsigned long)at.offset,(unsigned long)at.bufferIndex,
    (unsigned long)v.layouts[at.bufferIndex<31?at.bufferIndex:0].stride];
  }
  objc_setAssociatedObject(state,&gProbePipeKey,full,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
 }
 return (id<MTLRenderPipelineState>)state;
}
// The asynchronous forms make the pipeline on the caller's thread and call the handler before returning, as the library's
// newFunctionWithName:constantValues:completionHandler: does (RenderBox's pipeline loader, Core Image's bilateral solver).
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d completionHandler:(MTLNewRenderPipelineStateCompletionHandler)handler {
 NSError *error=nil;id<MTLRenderPipelineState> state=[(id<MTLDevice>)self newRenderPipelineStateWithDescriptor:d error:&error];
 handler(state,error);
}
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d options:(MTLPipelineOption)options
                           completionHandler:(MTLNewRenderPipelineStateWithReflectionCompletionHandler)handler {
 NSError *error=nil;id<MTLRenderPipelineState> state=[(id<MTLDevice>)self newRenderPipelineStateWithDescriptor:d options:options reflection:nil error:&error];
 handler(state,nil,error);
}
// dada (iOS 26): QuartzCore makes its compute pipelines with newComputePipelineStateWithDescriptor:error:. This file did not have it, and
// iOS's _MTLDevice answers it through the Apple driver's own internal path, which gives nil here: the render server then aborted
// ("Metal failed to build compute pipeline", g2vk4..6) with no failure of this file's own logged. The other device methods iOS 26's
// frameworks call that only the base class or Apple's driver had (test/selaudit), answered here.
- (id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d error:(NSError **)error {
 return [(id<MTLDevice>)self newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:error];
}
- (void)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d options:(MTLPipelineOption)options
                            completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)handler {
 NSError *error=nil;id<MTLComputePipelineState> state=[(id<MTLDevice>)self newComputePipelineStateWithDescriptor:d options:options reflection:nil error:&error];
 handler(state,nil,error);
}
- (void)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)handler {
 [self newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone completionHandler:handler];
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithTileDescriptor:(MTLTileRenderPipelineDescriptor *)d options:(MTLPipelineOption)options
                                                            reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection error:(NSError **)error {
 (void)options;if(reflection)*reflection=nil;
 return failWith<id<MTLRenderPipelineState>>(error,[NSString stringWithFormat:@"tile pipeline %@ (tile shaders are not supported)",d.tileFunction.name],nil);
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)surface plane:(NSUInteger)plane slice:(NSUInteger)slice {
 if(slice!=0){consoleLine([NSString stringWithFormat:@"surface texture slice %lu refused",(unsigned long)slice]);return nil;}
 return [(id<MTLDevice>)self newTextureWithDescriptor:d iosurface:surface plane:plane];
}
- (BOOL)supportsLossyCompression {return NO;}
- (BOOL)supportsDynamicLibraries {return NO;}
- (BOOL)supportsFunctionPointers {return NO;}
- (float)maxPointSize {return 511;}
- (NSString *)familyName {return @"Adreno";}
- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)error {
 (void)source;(void)options;
 return failWith<id<MTLLibrary>>(error,@"library from source (no Metal compiler on the device)",nil);
}
- (id<MTLSharedEvent>)newSharedEventWithHandle:(MTLSharedEventHandle *)handle {
 (void)handle;consoleLine(@"newSharedEventWithHandle: refused (no cross-process events)");return nil;
}
- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)function completionHandler:(MTLNewComputePipelineStateCompletionHandler)handler {
 NSError *error=nil;id<MTLComputePipelineState> state=[(id<MTLDevice>)self newComputePipelineStateWithFunction:function error:&error];
 handler(state,error);
}
- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)function options:(MTLPipelineOption)options
                          completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)handler {
 NSError *error=nil;id<MTLComputePipelineState> state=[(id<MTLDevice>)self newComputePipelineStateWithFunction:function options:options reflection:nil error:&error];
 handler(state,nil,error);
}
// SPI: a precompiled pipeline library (RenderBox.framework/default.pipelinelib) holds Apple GPU binaries. None is opened; the caller logs the
// error and builds its pipelines from the shader library instead (RB::Device::render_pipeline_state).
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error {
 if(error)*error=nbError([NSString stringWithFormat:@"no pipeline library on this GPU: %@",path.lastPathComponent]);
 return nil;
}
- (id<MTLBinaryArchive>)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)d error:(NSError **)error {
 if(d.url && !d.url.isFileURL){if(error)*error=nbError(@"a file URL is required");return nil;}
 NmbBinaryArchive *handle=nmb_binary_archive_new(gDevice,d.url?d.url.fileSystemRepresentation:nullptr);
 if(!handle)return failWith<id<MTLBinaryArchive>>(error,@"binary archive",nil);
 NBBinaryArchive *archive=[NBBinaryArchive new];archive.handle=handle;if(error)*error=nil;return (id<MTLBinaryArchive>)archive;
}
- (NSUInteger)requiredLinearTextureBytesPerRowForDescriptor:(MTLTextureDescriptor *)d {
 const uint64_t row=nmb_device_linear_bytes_per_row(gDevice,d.pixelFormat,d.width);
 if(!row)NBLOG(@"native-metal: requiredLinearTextureBytesPerRow: %@",lastError());
 return row;
}
- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d {
 NmbSamplerDescriptor s={(uint64_t)d.minFilter,(uint64_t)d.magFilter,(uint64_t)d.mipFilter,(uint64_t)d.sAddressMode,(uint64_t)d.tAddressMode,(uint64_t)d.rAddressMode,
     (uint64_t)d.borderColor,(uint64_t)d.normalizedCoordinates,d.maxAnisotropy,(uint64_t)d.compareFunction,(uint64_t)d.lodAverage,d.lodMinClamp,d.lodMaxClamp};
 if(pandoraAstcProbe()) {   // data-a-live: what QuartzCore samples with (tools/metal/adapt_ios16.py)
  static std::atomic<int> told{0};
  if(told.fetch_add(1)<40)NSLog(@"pandora-metal sampler: min %lu mag %lu mip %lu address %lu/%lu/%lu border %lu normalized %d lod %.1f-%.1f compare %lu",
   (unsigned long)d.minFilter,(unsigned long)d.magFilter,(unsigned long)d.mipFilter,(unsigned long)d.sAddressMode,(unsigned long)d.tAddressMode,
   (unsigned long)d.rAddressMode,(unsigned long)d.borderColor,(int)d.normalizedCoordinates,d.lodMinClamp,d.lodMaxClamp,(unsigned long)d.compareFunction);
 }
 NmbSampler *handle=nmb_sampler_new(gDevice,&s);
 if(!handle){NBLOG(@"native-metal: sampler refused: %@",lastError());return nil;}
 NBSampler *sampler=[NBSampler new];sampler.handle=handle;
 if(pandoraAstcProbe())objc_setAssociatedObject(sampler,&gProbeSamplerKey,[NSString stringWithFormat:@"min%lu mag%lu mip%lu addr%lu/%lu border%lu norm%d",
  (unsigned long)d.minFilter,(unsigned long)d.magFilter,(unsigned long)d.mipFilter,(unsigned long)d.sAddressMode,(unsigned long)d.tAddressMode,
  (unsigned long)d.borderColor,(int)d.normalizedCoordinates],OBJC_ASSOCIATION_RETAIN_NONATOMIC);
 return (id<MTLSamplerState>)sampler;
}
- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)d {
 auto face=[](MTLStencilDescriptor *s)->NmbStencil {
  return {(uint64_t)s.stencilCompareFunction,(uint64_t)s.stencilFailureOperation,(uint64_t)s.depthFailureOperation,(uint64_t)s.depthStencilPassOperation,s.readMask,s.writeMask};
 };
 NmbDepthStencilDescriptor s={(uint64_t)d.depthCompareFunction,(uint64_t)d.depthWriteEnabled,face(d.frontFaceStencil),face(d.backFaceStencil)};
 NmbDepthStencil *handle=nmb_depth_stencil_new(gDevice,&s);
 if(!handle){NBLOG(@"native-metal: depth/stencil state refused: %@",lastError());return nil;}
 NBDepthStencil *state=[NBDepthStencil new];state.handle=handle;
 if(pandoraAstcProbe())objc_setAssociatedObject(state,&gProbeDepthKey,[NSString stringWithFormat:@"depth compare %lu write %d stencil front %lu/%lu/%lu/%lu back %lu/%lu/%lu/%lu",
  (unsigned long)d.depthCompareFunction,(int)d.depthWriteEnabled,(unsigned long)d.frontFaceStencil.stencilCompareFunction,
  (unsigned long)d.frontFaceStencil.stencilFailureOperation,(unsigned long)d.frontFaceStencil.depthFailureOperation,
  (unsigned long)d.frontFaceStencil.depthStencilPassOperation,(unsigned long)d.backFaceStencil.stencilCompareFunction,
  (unsigned long)d.backFaceStencil.stencilFailureOperation,(unsigned long)d.backFaceStencil.depthFailureOperation,
  (unsigned long)d.backFaceStencil.depthStencilPassOperation],OBJC_ASSOCIATION_RETAIN_NONATOMIC);
 return (id<MTLDepthStencilState>)state;
}
@end

// The device queries the runtime answers (nmb_device_answer_selector), as methods of the device class. One without an argument is read once
// and installed only where the device has an answer of its own; one with an argument asks the runtime each call, and a refusal answers 0/NO
// with the reason logged.
static std::unordered_map<SEL,uint64_t> gAnswers;
static NSUInteger countAnswer(id,SEL cmd) {return (NSUInteger)gAnswers.at(cmd);}
static BOOL flagAnswer(id,SEL cmd) {return gAnswers.at(cmd)!=0;}
static uint64_t askAnswer(SEL cmd,NSUInteger arg) {
 static std::mutex lock;   // data-a-live: each (selector, argument) asked once (tools/metal/adapt_ios16.py, round 2)
 static std::unordered_map<uint64_t,std::vector<std::pair<SEL,uint64_t>>> known;   // argument -> (selector, answer)
 {std::lock_guard<std::mutex> hold(lock);
  if(auto at=known.find(arg);at!=known.end())for(const auto &seen:at->second)if(seen.first==cmd)return seen.second;}
 uint64_t value=0;
 if(!nmb_device_answer(gDevice,sel_getName(cmd),arg,&value)){NBLOG(@"native-metal: %s(%lu): %@",sel_getName(cmd),(unsigned long)arg,lastError());value=0;}
 std::lock_guard<std::mutex> hold(lock);
 known[arg].emplace_back(cmd,value);
 return value;
}
static NSUInteger countArgAnswer(id,SEL cmd,NSUInteger arg) {return (NSUInteger)askAnswer(cmd,arg);}
static BOOL flagArgAnswer(id,SEL cmd,NSUInteger arg) {return askAnswer(cmd,arg)!=0;}
static BOOL deviceResolve(id cls,SEL,SEL sel) {return nbAddDefault((Class)cls,sel);}
static void installAnswers(Class cls) {
 uint32_t kind=0;
 for(size_t i=0;const char *name=nmb_device_answer_selector(i,&kind);i++) {
  SEL sel=sel_registerName(name);uint64_t value=0;
  switch(kind) {
  case NMB_ANSWER_COUNT: case NMB_ANSWER_FLAG:
   if(!nmb_device_answer(gDevice,name,0,&value)){NBLOG(@"native-metal: no device answer: %@",lastError());continue;}
   // Extended-range 10-bit (XR10) framebuffers are withheld: this display chain presents 8-bit BGRX scanout only. The render server picks its
   // wide-gamut 64-bit (and Apple-compressed) framebuffer when a device offers them, and the display driver could not show it (int-089: the
   // swap's surface was '&w4a', 8 bytes per pixel, and every swap was refused).
   // dada: withheld from the render server only (its framebuffer choice); a client's CAMetalLayer with an extended-range format is
   // refused by QuartzCore on a device that says no ("CAMetalLayerInvalid: unsupported extended range format": AegirPoster, the
   // default iOS 26 wallpaper, aborted on it in every run)
   if(!strcmp(name,"supportsExtendedXR10Formats") && !strcmp(getprogname(),"backboardd"))value=0;
   gAnswers[sel]=value;
   class_addMethod(cls,sel,kind==NMB_ANSWER_COUNT?(IMP)countAnswer:(IMP)flagAnswer,kind==NMB_ANSWER_COUNT?"Q16@0:8":"B16@0:8");
   break;
  case NMB_ANSWER_COUNT_ARG: class_addMethod(cls,sel,(IMP)countArgAnswer,"Q24@0:8Q16");break;
  case NMB_ANSWER_FLAG_ARG: class_addMethod(cls,sel,(IMP)flagArgAnswer,"B24@0:8Q16");break;
  }
 }
}

// dada: boot-args nbcap=N nbcap_proc=PREFIX: the first N capture lines (passes with their load actions and clear colours, pipelines,
// textures, draws) of the processes whose names begin with PREFIX (all without it), on the console -- the lock screen's wallpaper
// left the regions of its background white with alpha 0 (g2k59), and which pass should have drawn them is the question.
static void captureFromBootArgs() {
 char args[1024]={};size_t size=sizeof args-1;
 if(sysctlbyname("kern.bootargs",args,&size,nullptr,0)!=0)return;
 const char *n=strstr(args,"nbcap=");if(!n)return;
 const char *proc=strstr(args,"nbcap_proc=");
 if(proc){char want[64]={};snprintf(want,sizeof want,"%.*s",(int)strcspn(proc+11," "),proc+11);if(strncmp(getprogname(),want,strlen(want))!=0)return;}
 const int lines=atoi(n+6);
 if(lines>0){gCapture.store(lines);NSLog(@"pandora-metal cap: capturing %d lines in %s",lines,getprogname());}
}
extern "C" id native_bridge_metal_open(const char *loader,const char *cache,uint64_t registryID,void (*missing)(const char *,const char *)) {
 captureFromBootArgs();
 if(gDeviceObject)return gDeviceObject;
 gMissing=missing;gInFlight=[NSMutableSet set];
 gDevice=nmb_device_open(loader,cache);
 if(!gDevice){NBLOG(@"native-metal: device refused: %s",nmb_last_error());return nil;}
 if(cache){gJournalPath=std::string(cache)+"/pipelines.log";gJournalBegan=nowUs();}
 char name[128]={};nmb_device_name(gDevice,name,sizeof name);gName=[NSString stringWithUTF8String:name];gRegistryID=registryID;
 // The target families supportsFamily: answers true, with the rows of Apple's tables the device does not meet in them (nmb_device_family_waivers).
 {char waived[4096]={};nmb_device_family_waivers(gDevice,waived,sizeof waived);
  for(char *line=waived,*next;*line;line=next){next=strchr(line,'\n');if(next)*next++=0;else next=line+strlen(line);journal(std::string("F ")+line);}}
 Class base=objc_getClass("_MTLDevice");
#if !TARGET_OS_IPHONE
 // Host instrument: on NSObject every selector a client sends that this file lacks reaches the missing hook, instead of the host's
 // own _MTLDevice implementation.
 if(getenv("NMB_DEVICE_BASE_NSOBJECT"))base=[NSObject class];
#endif
#if TARGET_OS_IPHONE
 // The target's base class ABI (iPadOS 17.7.11): the dynamic subclass adds no instance variables.
 // data-a-live: 528 bytes on iOS 16.7 (ios-fuxi docs-lanes/metal.md), 536 on iPadOS 17.7.11.
 // dada (iOS 26): the dynamic subclass adds no instance variables, so the base's size is only reported, not required (536 on iPadOS 17.7.11,
 // 528 on iOS 16.7).
 if(!base){NBLOG(@"native-metal: no _MTLDevice");return nil;}
 {static bool said;if(!said){said=true;NSLog(@"gpu2-metal: _MTLDevice is %zu bytes",class_getInstanceSize(base));}}
#endif
 if(!base)base=[NSObject class];
 Class cls=objc_allocateClassPair(base,"NativeBridgeMetalDevice",0);if(!cls)return nil;
 unsigned count=0;Method *methods=class_copyMethodList([NBDeviceMethods class],&count);
 for(unsigned i=0;i<count;i++)class_addMethod(cls,method_getName(methods[i]),method_getImplementation(methods[i]),method_getTypeEncoding(methods[i]));
 free(methods);installAnswers(cls);class_addProtocol(cls,@protocol(MTLDevice));
 class_addMethod(object_getClass(cls),@selector(resolveInstanceMethod:),(IMP)deviceResolve,"B24@0:8:16");   // dada: NBDefaults.h
 // The original registration (_MTLAddDevice) asserts that a plugin's device conforms to Metal's private device protocol as well; the target's own
 // drivers get it from their IOGPU base class. Conformance only answers conformsToProtocol:, the methods are still looked up one by one.
 if(Protocol *spi=objc_getProtocol("MTLDeviceSPI"))class_addProtocol(cls,spi);
 objc_registerClassPair(cls);
 gDeviceObject=[cls new];
#if TARGET_OS_IPHONE
 // dada: why a Metal client stops, on the console: its uncaught exception, and its stderr (assertions, abort messages) when that goes
 // nowhere. Clients' os_log does not reach the console here.
 {struct stat st;if(fstat(2,&st)!=0||(S_ISCHR(st.st_mode)&&major(st.st_rdev)==3&&minor(st.st_rdev)==2)){
   int fd=open("/dev/console",O_WRONLY|O_NOCTTY|O_CLOEXEC);if(fd>=0){dup2(fd,2);close(fd);}}}
 static NSUncaughtExceptionHandler *previous=NSGetUncaughtExceptionHandler();
 NSSetUncaughtExceptionHandler([](NSException *e){
  consoleLine([NSString stringWithFormat:@"uncaught %@: %@ | %@",e.name,e.reason,[[e.callStackSymbols subarrayWithRange:NSMakeRange(0,MIN((NSUInteger)8,e.callStackSymbols.count))] componentsJoinedByString:@" ; "]]);
  if(previous)previous(e);});
 // dada: where a crash happened, as image+offset (the kernel's crash line gives an address only: g2k33's backboardd died of a read at
 // 0x1b20 in some library), written straight to the console from the handler; then the signal's own action.
 {struct sigaction sa={};
  sa.sa_sigaction=[](int sig,siginfo_t *info,void *context){
   char line[512];int at=snprintf(line,sizeof line,"gpu2-metal[%s %d]: signal %d at %p:",getprogname(),getpid(),sig,info?info->si_addr:nullptr);
   const ucontext_t *uc=(const ucontext_t *)context;
   uintptr_t pcs[2]={uc?(uintptr_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss):0,uc?(uintptr_t)__darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss):0};
   for(int k=0;k<2 && at<(int)sizeof line-120;k++){
    const uintptr_t a=pcs[k]&0x0000007fffffffffULL;Dl_info di{};
    if(a && dladdr((void *)a,&di) && di.dli_fname){const char *slash=strrchr(di.dli_fname,'/');
     at+=snprintf(line+at,sizeof line-at," %s %s+%#lx (%s+%#lx)",k?"lr":"pc",slash?slash+1:di.dli_fname,(unsigned long)(a-(uintptr_t)di.dli_fbase),
      di.dli_sname?di.dli_sname:"?",(unsigned long)(di.dli_saddr?a-(uintptr_t)di.dli_saddr:0));}
    else at+=snprintf(line+at,sizeof line-at," %s %#lx",k?"lr":"pc",(unsigned long)a);
   }
   line[at++]='\n';
   int fd=open("/dev/console",O_WRONLY|O_NOCTTY);if(fd>=0){(void)write(fd,line,at);close(fd);}
   signal(sig,SIG_DFL);raise(sig);};
  sa.sa_flags=SA_SIGINFO|SA_ONSTACK;sigemptyset(&sa.sa_mask);
  static bool installed;if(!installed){installed=true;sigaction(SIGSEGV,&sa,nullptr);sigaction(SIGBUS,&sa,nullptr);}}
 consoleLine([NSString stringWithFormat:@"device %@ open",gName]);
#endif
 return gDeviceObject;
}

// Bare AIR bitcode (no metallib wrapper), which Metal itself refuses: for the bridge's own fixtures only.
extern "C" id native_bridge_metal_library_with_air(const void *bytes,size_t length) {
 NmbLibrary *handle=gDevice?nmb_library_new(gDevice,bytes,length,true):nullptr;if(!handle)return nil;
 NBLibrary *library=[NBLibrary new];library.handle=handle;return library;
}
extern "C" bool native_bridge_metal_shader_cache_stats(NmbShaderCacheStats *out) {
 return gDevice && nmb_device_shader_cache_stats(gDevice,out);
}
extern "C" bool native_bridge_metal_work_stats(uint64_t out[10]) {
 return gDevice && nmb_device_work_stats(gDevice,out);
}

extern "C" size_t native_bridge_metal_status(char *out,size_t size) {
 std::lock_guard<std::mutex> lock(gProblemLock);
 return (size_t)snprintf(out,size,"c%llu/%llu d%llu p%llu x%llu R%llu D%llu F%llu M%llu C%llu B%llu %s | %s",(unsigned long long)gCommits.load(),(unsigned long long)gCommitFailures.load(),(unsigned long long)gCompleted.load(),
  (unsigned long long)gPipelines.load(),(unsigned long long)gProblems.load(),(unsigned long long)gPasses.load(),(unsigned long long)gDraws.load(),(unsigned long long)gFullScreenPasses.load(),(unsigned long long)(gBiggestPass.load()>>10),(unsigned long long)gDispatches.load(),(unsigned long long)gBlits.load(),gLastProblems[0],gLastProblems[1]);
}

static size_t native_bridge_metal_formats_locked(char *out,size_t size) {
 int n=snprintf(out,size,"tex x%llu s%llu/%llu p%llu f",(unsigned long long)gTextureRefusals.load(),(unsigned long long)gSurfaceTextures.load(),
              (unsigned long long)gSurfaceRefusals.load(),(unsigned long long)gSurfacePlanar.load());
 for(unsigned i=0;i<gFormatCount && n>0 && (size_t)n<size;i++)n+=snprintf(out+n,size-(size_t)n,"%u ",gFormats[i]);
 return n>0?(size_t)n:0;
}
// The benchmark's rows (native_bridge_metal_benchmark), kept for the telemetry table.
static std::mutex gBenchLock;
static char gBench[4][96];
static std::atomic<unsigned> gBenchRuns;
extern "C" size_t native_bridge_metal_benchmark(id device,const char *metallib,unsigned pairs,char *out,size_t size) {
 auto now=[]{return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);};
 // the first run fills rows 0-1 (Y, Y1), a later one rows 2-3 (Z, Z1), so a busy and an idle run are both kept
 const unsigned base=gBenchRuns++?2u:0u;
 auto put=[&](unsigned row,const char *format,auto... args){std::lock_guard<std::mutex> lock(gBenchLock);snprintf(gBench[base+row],sizeof gBench[base+row],format,args...);
  if(base)gBench[base+row][0]='Z';};
 // compute: a dependent integer chain (no memory), 100M steps
 uint64_t t=now(),x=0x9e3779b97f4a7c15ull;
 for(uint32_t i=0;i<100000000u;i++){x^=x<<13;x^=x>>7;x^=x<<17;}
 const uint64_t computeUs=(now()-t)/1000;
 // first touch: 32 MiB of fresh anonymous memory (the tablet's free memory is small), one store per 4 KiB page (the kernel's fault and zero-fill), then four memsets of it
 const size_t bytes=32u<<20;
 uint64_t faultUs=0,fillUs=0,shaUs=0;
 void *m=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
 if(m!=MAP_FAILED) {
  volatile uint8_t *b=(volatile uint8_t *)m;
  t=now();for(size_t at=0;at<bytes;at+=4096)b[at]=1;faultUs=(now()-t)/1000;
  t=now();for(int k=0;k<4;k++)memset(m,k,bytes);fillUs=(now()-t)/1000;
  unsigned char digest[CC_SHA256_DIGEST_LENGTH];
  t=now();CC_SHA256(m,(CC_LONG)(16u<<20),digest);shaUs=(now()-t)/1000;
  x^=digest[0];
  munmap(m,bytes);
 }
 put(0,"Y cpu=%llums(100M) fault=%lluus/32MiB=%lluns/pg fill=%lluMB/s sha=%lluMB/s %llx",(unsigned long long)(computeUs/1000),
  (unsigned long long)faultUs,(unsigned long long)(faultUs*1000/8192),(unsigned long long)(fillUs?4ull*32*1000000/fillUs:0),
  (unsigned long long)(shaUs?16ull*1000000/shaUs:0),(unsigned long long)(x&0xf));
 // pipelines: the host tool's pairs, in its order
 uint64_t made=0,refused=0,createUs=0,createMax=0,drawUs=0,drawMax=0,libraryUs=0;
 @autoreleasepool {
  id<MTLDevice> dev=(id<MTLDevice>)device;NSError *error=nil;
  t=now();
  id<MTLLibrary> qc=metallib?[dev newLibraryWithURL:[NSURL fileURLWithPath:@(metallib)] error:&error]:nil;
  libraryUs=(now()-t)/1000;
  if(qc) {
   NSArray<NSString *> *names=qc.functionNames;
   id<MTLCommandQueue> queue=[dev newCommandQueue];
   MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
   td.usage=MTLTextureUsageRenderTarget;
   id<MTLTexture> target=[dev newTextureWithDescriptor:td];
   id<MTLBuffer> zeros=[dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
   unsigned n=0;
   for(NSString *name in names) {
    if(n>=pairs)break;
    id<MTLFunction> f=[qc newFunctionWithName:name];
    if(f.functionType!=MTLFunctionTypeVertex)continue;
    NSString *fragmentName=[name stringByReplacingOccurrencesOfString:@"_vert_" withString:@"_frag_"];
    if([fragmentName isEqualToString:name] || ![names containsObject:fragmentName])continue;
    ++n;
    MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new];
    d.vertexFunction=f;d.fragmentFunction=[qc newFunctionWithName:fragmentName];
    d.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
    t=now();
    id<MTLRenderPipelineState> state=[dev newRenderPipelineStateWithDescriptor:d error:&error];
    const uint64_t took=(now()-t)/1000;
    if(!state){++refused;continue;}
    ++made;createUs+=took;createMax=std::max(createMax,took);
    t=now();
    MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture=target;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    id<MTLCommandBuffer> commands=[queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder=[commands renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:state];
    for(NSUInteger i=0;i<8;++i){[encoder setVertexBuffer:zeros offset:0 atIndex:i];[encoder setFragmentBuffer:zeros offset:0 atIndex:i];}
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];[commands commit];[commands waitUntilCompleted];
    const uint64_t drew=(now()-t)/1000;
    drawUs+=drew;drawMax=std::max(drawMax,drew);
   }
  }
 }
 put(1,"Y1 qc lib=%llums made=%llu refused=%llu create=%llums max=%llums draw=%llums max=%llums",(unsigned long long)(libraryUs/1000),
  (unsigned long long)made,(unsigned long long)refused,(unsigned long long)(createUs/1000),(unsigned long long)(createMax/1000),
  (unsigned long long)(drawUs/1000),(unsigned long long)(drawMax/1000));
 size_t at=0;
 if(out && size) {
  std::lock_guard<std::mutex> lock(gBenchLock);
  out[0]=0;for(unsigned r=base;r<base+2;r++)at+=(size_t)snprintf(out+at,size>at?size-at:0,"%s\n",gBench[r]);
 }
 return at;
}
// The telemetry table the plugin streams to the kernel's log: newline-separated rows of at most 90 characters.
extern "C" size_t native_bridge_metal_telemetry(char *out,size_t size) {
 size_t at=0;out[0]=0;
 auto row=[&](const char *format,auto... args){
  char text[200];int n=snprintf(text,sizeof text,format,args...);if(n<=0)return;if(n>90)n=90;
  if(at+(size_t)n+2>=size)return;memcpy(out+at,text,(size_t)n);at+=(size_t)n;out[at++]='\n';out[at]=0;};
 auto ms=[](uint64_t us){return (unsigned long long)(us/1000);};
 {std::lock_guard<std::mutex> lock(gBenchLock);for(unsigned r=0;r<4;r++)if(gBench[r][0])row("%.90s",gBench[r]);}
 row("S c%llu/%llu d%llu p%llu x%llu R%llu D%llu F%llu M%lluk C%llu B%llu",(unsigned long long)gCommits.load(),(unsigned long long)gCommitFailures.load(),
  (unsigned long long)gCompleted.load(),(unsigned long long)gPipelines.load(),(unsigned long long)gProblems.load(),(unsigned long long)gPasses.load(),
  (unsigned long long)gDraws.load(),(unsigned long long)gFullScreenPasses.load(),(unsigned long long)(gBiggestPass.load()>>10),
  (unsigned long long)gDispatches.load(),(unsigned long long)gBlits.load());
 row("T pipeline n=%llu total=%llums max=%llums",(unsigned long long)gTimePipeline.count.load(),ms(gTimePipeline.total.load()),ms(gTimePipeline.slowest.load()));
 row("T library n=%llu total=%llums max=%llums",(unsigned long long)gTimeLibrary.count.load(),ms(gTimeLibrary.total.load()),ms(gTimeLibrary.slowest.load()));
 row("T function n=%llu total=%llums max=%llums",
  (unsigned long long)gTimeFunction.count.load(),ms(gTimeFunction.total.load()),ms(gTimeFunction.slowest.load()));
 row("G commit-to-complete n=%llu total=%llums max=%llums mean=%lluus",(unsigned long long)gTimeEncodeToCommit.count.load(),ms(gTimeEncodeToCommit.total.load()),
  ms(gTimeEncodeToCommit.slowest.load()),(unsigned long long)(gTimeEncodeToCommit.count.load()?gTimeEncodeToCommit.total.load()/gTimeEncodeToCommit.count.load():0));
 row("J scheduled wait n=%llu total=%llums max=%llums",(unsigned long long)gTimeWaitScheduled.count.load(),ms(gTimeWaitScheduled.total.load()),ms(gTimeWaitScheduled.slowest.load()));
 row("J completed wait n=%llu total=%llums max=%llums",(unsigned long long)gTimeWaitCompleted.count.load(),ms(gTimeWaitCompleted.total.load()),ms(gTimeWaitCompleted.slowest.load()));
 row("J resource wait n=%llu total=%llums max=%llums",(unsigned long long)gTimeResourceWait.count.load(),ms(gTimeResourceWait.total.load()),ms(gTimeResourceWait.slowest.load()));
 row("J resource checks=%llu skipped=%llu",(unsigned long long)gResourceChecks.load(),(unsigned long long)gResourceSkipped.load());
 row("J surface cache n=%llu bytes=%llu dup=%llu us=%llu max=%llu",(unsigned long long)gTimeSurfaceFlush.count.load(),
  (unsigned long long)gSurfaceFlushBytes.load(),(unsigned long long)gSurfaceFlushDuplicates.load(),
  (unsigned long long)gTimeSurfaceFlush.total.load(),(unsigned long long)gTimeSurfaceFlush.slowest.load());
 row("J mirrors copied=%llu bytes=%llu us=%llu max=%llu",
  (unsigned long long)gMirrorBuffersCopied.load(),
  (unsigned long long)gMirrorBytesSynced.load(),(unsigned long long)gTimeMirrorSync.total.load(),
  (unsigned long long)gTimeMirrorSync.slowest.load());
 row("J parallel batches=%llu buffers=%llu workers=3",(unsigned long long)gMirrorParallelBatches.load(),
  (unsigned long long)gMirrorParallelBuffers.load());
 row("J snapshot bytes=%llu fallback=%llu",(unsigned long long)gMirrorSnapshotBytes.load(),
  (unsigned long long)gMirrorSnapshotFallbacks.load());
 row("N no-copy %llu dealloc %llu/%llu surface %llu/%llu",(unsigned long long)gNoCopyAll.load(),(unsigned long long)gNoCopyReleased.load(),
  (unsigned long long)gNoCopyMade.load(),
  (unsigned long long)gSurfaceImportsReleased.load(),(unsigned long long)gSurfaceImportsMade.load());
 // The runtime's VkPipeline makings (the creation log, METAL2VULKAN_LOG_PIPELINES; they happen at draws, not at the state's creation) and
 // its shader cache: where the driver's compile time goes.
 uint64_t work[10]={};
 if(gDevice && nmb_device_work_stats(gDevice,work)) {
  row("V vkpipe n=%llu total=%llums max=%llums waited=%llums failed=%llu",
   (unsigned long long)work[0],ms(work[1]),ms(work[2]),ms(work[3]),(unsigned long long)work[4]);
  row("V cache hit%llu miss%llu wr%llu | msg i%llu w%llu",(unsigned long long)work[7],(unsigned long long)work[8],(unsigned long long)work[9],(unsigned long long)work[5],
   (unsigned long long)work[6]);
  char warning[200];size_t n=nmb_last_warning(warning,sizeof warning);
  if(n){row("W %.86s",warning);if(n>86)row("Wb %.86s",warning+86);}
 }
 {
  std::lock_guard<std::mutex> lock(gProblemLock);
  char formats[160];native_bridge_metal_formats_locked(formats,sizeof formats);row("F %.86s",formats);
  const SurfaceRefusal &r=gSurfaceLastRefusal;
  if(r.step)row("SR %u %08x %c%c%c%c pl%u/%u o%llx rb%llu a%llx %llux%llu pf%u",r.step,r.format,(char)(r.format>>24),(char)(r.format>>16),(char)(r.format>>8),
   (char)r.format,r.plane,r.planes,(unsigned long long)r.offset,(unsigned long long)r.row,(unsigned long long)r.allocation,(unsigned long long)r.width,
   (unsigned long long)r.height,r.pixelFormat);
  row("P0 %.86s",gLastProblems[0]);row("P0b %.86s",strlen(gLastProblems[0])>86?gLastProblems[0]+86:"");
  row("P1 %.86s",gLastProblems[1]);row("P1b %.86s",strlen(gLastProblems[1])>86?gLastProblems[1]+86:"");
 }
 {
  std::lock_guard<std::mutex> lock(gCommitLock);
  const uint32_t newest=(uint32_t)gCommits.load();
  row("%s","Q timing v2: per-buffer API counts; ctc=commit-to-callback; age includes caller idle");
  for(uint32_t k=0;k<20 && k<newest;k++) {
   const CommitRecord &r=gCommitRing[(newest-k)%32];
   if(r.index!=newest-k)continue;
   row("K%u t=%llu.%03llus rp%u dr%u kpx%llu cs%u bl%u ctc=%lluus",r.index,(unsigned long long)(r.committedUs/1000000),(unsigned long long)((r.committedUs/1000)%1000),
    r.work.passes,r.work.draws,(unsigned long long)(r.work.pixels>>10),r.work.dispatches,r.work.blits,(unsigned long long)r.callbackUs);
   if(k<4)row("E%u age=%lluus scheduled=%lldus",r.index,(unsigned long long)r.ageUs,(long long)r.scheduledUs);
  }
 }
 return at;
}

extern "C" uint64_t native_bridge_metal_first_problem(char *out,size_t size) {
 std::lock_guard<std::mutex> lock(gProblemLock);
 if(size)strlcpy(out,gFirstProblem,size);
 return gProblems.load();
}
extern "C" size_t native_bridge_metal_formats(char *out,size_t size) {
 std::lock_guard<std::mutex> lock(gProblemLock);
 return native_bridge_metal_formats_locked(out,size);
}

// data-a-live: the bridge's time since the last call, as text (tools/metal/adapt_ios16.py, 2026-10-25): the share of
// one CPU inside the bridge and in its blocking waits, then the busiest functions; then Turnip's render pass counts.
extern "C" size_t native_bridge_metal_pandora_timing(char *out,size_t size) {
 static uint64_t last,lastOuter,lastWaits,shownCalls[PANDORA_BT_COUNT],shownNs[PANDORA_BT_COUNT];
 const uint64_t now=clock_gettime_nsec_np(CLOCK_UPTIME_RAW),span=last?now-last:0;
 last=now;
 const uint64_t outer=gPandoraBridgeOuter.load(),waits=gPandoraBridgeWaits.load();
 const uint64_t dOuter=outer-lastOuter,dWaits=waits-lastWaits;
 lastOuter=outer;lastWaits=waits;
 uint64_t calls[PANDORA_BT_COUNT],ns[PANDORA_BT_COUNT];
 unsigned order[PANDORA_BT_COUNT];
 for(unsigned i=0;i<PANDORA_BT_COUNT;i++){
  const uint64_t c=gPandoraBridge[i].calls.load(),t=gPandoraBridge[i].ns.load();
  calls[i]=c-shownCalls[i];ns[i]=t-shownNs[i];shownCalls[i]=c;shownNs[i]=t;order[i]=i;
 }
 std::sort(order,order+PANDORA_BT_COUNT,[&](unsigned a,unsigned b){return ns[a]>ns[b];});
 int at=snprintf(out,size,"bridge %llu.%llu%% of one CPU over %llu ms, waits %llu ms (1 call in 16 timed)\n",
  (unsigned long long)(span?dOuter*100/span:0),(unsigned long long)(span?dOuter*1000/span%10:0),
  (unsigned long long)(span/1000000),(unsigned long long)(dWaits/1000000));
 {  // dada: the work of the interval (where the render server's frames go: g2k27 took 13-44 ms of GPU a submission)
  static uint64_t lastCommits,lastPasses,lastFull,lastDraws,lastPixels,lastBlits,lastDispatches;
  const uint64_t c=gCommits.load(),p=gPasses.load(),f=gFullScreenPasses.load(),dr=gDraws.load(),px=gPassPixels.load(),b=gBlits.load(),di=gDispatches.load();
  if(at>0 && (size_t)at<size)
   at+=snprintf(out+at,size-at,"  work: %llu commits, %llu passes (%llu full-screen, %llu Mpixels), %llu draws, %llu blits, %llu dispatches\n",
    (unsigned long long)(c-lastCommits),(unsigned long long)(p-lastPasses),(unsigned long long)(f-lastFull),(unsigned long long)((px-lastPixels)>>20),
    (unsigned long long)(dr-lastDraws),(unsigned long long)(b-lastBlits),(unsigned long long)(di-lastDispatches));
  lastCommits=c;lastPasses=p;lastFull=f;lastDraws=dr;lastPixels=px;lastBlits=b;lastDispatches=di;
  std::vector<std::pair<uint64_t,uint64_t>> rows;   // pixels, key
  std::map<uint64_t,FormatWork> work;
  {std::lock_guard<std::mutex> lock(gFormatLock);work.swap(gFormatWork);}
  for(auto &kv:work)rows.push_back({kv.second.pixels,kv.first});
  std::sort(rows.rbegin(),rows.rend());
  for(size_t i=0;i<rows.size() && i<5 && at>0 && (size_t)at<size;i++) {
   const uint64_t k=rows[i].second;const FormatWork &w=work[k];
   at+=snprintf(out+at,size-at,"    pf %llu load %llu%s: %llu passes, %llu Mpixels\n",(unsigned long long)(k>>8),(unsigned long long)((k>>4)&15),
    (k&1)?" msaa":"",(unsigned long long)w.passes,(unsigned long long)(w.pixels>>20));
  }
 }
 if(at>0 && (size_t)at<size)
  at+=snprintf(out+at,size-at,"  big textures: %lld live (%lld views, %lld volatile, ~%lld MB), %lld made and %lld freed since\n",
   (long long)gBigLive.load(),(long long)gBigViews.load(),(long long)gBigVolatile.load(),(long long)(gBigBytes.load()>>20),
   (long long)gBigMade.exchange(0),(long long)gBigFreed.exchange(0));
 if(gBigLive.load()>=20) {   // the makers of the live ones, most first
  std::vector<std::pair<int64_t,std::string>> rows;
  {std::lock_guard<std::mutex> lock(gBigLock);if(gBigCallers)for(auto &kv:*gBigCallers)rows.push_back({kv.second,kv.first});}
  std::sort(rows.rbegin(),rows.rend());
  for(size_t i=0;i<rows.size() && i<5 && at>0 && (size_t)at<size;i++)
   at+=snprintf(out+at,size-at,"    %lld live from %s\n",(long long)rows[i].first,rows[i].second.c_str());
 }
 for(unsigned k=0;k<14 && at>0 && (size_t)at<size && ns[order[k]];k++){
  const unsigned i=order[k];
  at+=snprintf(out+at,size-at,"  %s: %llu calls, %llu us (%llu ns each)\n",gPandoraBridgeNames[i],(unsigned long long)calls[i],
   (unsigned long long)(ns[i]/1000),(unsigned long long)(calls[i]?ns[i]/calls[i]:0));
 }
 static size_t (*stats)(char *,size_t);
 static bool looked;
 if(!looked){
  looked=true;
  for(const char *path:{"/usr/lib/libvulkan_freedreno.dylib",(const char *)nullptr}){
   void *image=path?dlopen(path,RTLD_NOLOAD|RTLD_LAZY):RTLD_DEFAULT;
   if(image && (stats=(size_t (*)(char *,size_t))dlsym(image,"tu_pandora_render_stats")))break;
  }
 }
 if(at>0 && (size_t)at<size){
  if(stats){at+=snprintf(out+at,size-at,"turnip: ");if((size_t)at<size)at+=(int)stats(out+at,size-at);}
  else at+=snprintf(out+at,size-at,"turnip: no render pass counts (tu_pandora_render_stats not found)");
 }
 return at>0?std::min((size_t)at,size?size-1:0):0;
}

// data-a-live: the runtime's work counts that changed since the last call (NativeMetalObjects.rs nmb_device_work_counts).
extern "C" size_t native_bridge_metal_pandora_work(char *out,size_t size) {
 if(!gDevice){if(size)*out=0;return 0;}
 return nmb_device_work_counts(gDevice,out,size);
}
