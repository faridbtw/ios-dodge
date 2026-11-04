// SPDX-License-Identifier: MIT
// Adreno Metal plugin, reached through the original discovery (IOAcceleratorES -> MetalPluginName -> NSBundle -> principal class ->
// initWithAcceleratorPort:). Default (stage 1): it runs the shared userspace GPU test (NativeAdrenoUserTest.h) from inside a real system
// process, under that process's sandbox, and then returns nil. No device is registered, so the original software rendering keeps running unchanged.
// Published (the image carries the file NativeAdrenoMetal.publish beside the bundle): backboardd, the one process that owns the GPU, returns the native
// Metal frontend's device (the metal2vulkan runtime on Turnip, on the Adreno 740), which the original discovery registers; the CARenderServer then
// renders the system UI through it, and the display driver's frames are written to the scanout by the GPU (GpuScanoutClient.h). Another process
// gets its own native device (its own driver connection, GPU address window and shader cache) when its name is listed in the file
// NativeAdrenoMetal.clients beside the bundle ("*" lists every process); an unlisted one returns nil and keeps the software renderer.
#import <Foundation/Foundation.h>
#include <pthread.h>
#include <sys/sysctl.h>
extern "C" size_t native_bridge_metal_pandora_timing(char *out, size_t size);   // data-a-live (NativeBridgeMetalFrontend.mm)
extern "C" size_t native_bridge_metal_pandora_work(char *out, size_t size);     // data-a-live (NativeBridgeMetalFrontend.mm)
#include <dlfcn.h>
#include <unistd.h>
#include <pwd.h>
#include <mach/mach.h>
#include <objc/message.h>
#include "NativeAdrenoUserTest.h"
#include "NativeMetalBridgePhase.h"
#include "CacheExport.h"
#include "PrimeShaders.h"
#include "ExceptionDiag.h"

@interface _MTLDevice : NSObject
@end
@interface NativeAdrenoMetalDeviceBase : NSObject   // only a lock object for the published mode's one-time setup
@end
@implementation NativeAdrenoMetalDeviceBase
@end
@interface NativeAdrenoMetalDevice : _MTLDevice
- (id)initWithAcceleratorPort:(unsigned)port;
@end

// ---- published mode ----
namespace publish {
// The image's marker: a file beside the bundle in the union view of /System/Library/Extensions (the bundle's own seal stays untouched).
// A file beside the bundle: its path in path, true when it is readable.
static bool requestedPath(const char *name, char *path, size_t size) {
    Dl_info info = {};
    if (!dladdr(reinterpret_cast<const void *>(&requestedPath), &info) || !info.dli_fname) return false;
    snprintf(path, size, "%s", info.dli_fname);
    char *slash = strrchr(path, '/');          // the bundle's executable: <dir>/NativeAdrenoMetal.bundle/NativeAdrenoMetal
    if (!slash) return false;
    *slash = 0;
    slash = strrchr(path, '/');
    if (!slash) return false;
    snprintf(slash + 1, size - size_t(slash + 1 - path), "%s", name);
    if (access(path, R_OK) == 0) return true;
    // dada: /System/Library/Extensions/RedwoodMetal.bundle is a link to /redwood/lib/RedwoodMetal.bundle; the files beside it are there
    snprintf(path, size, "/redwood/lib/%s", name);
    return access(path, R_OK) == 0;
}
static bool requested() {
    char path[1024];
    return requestedPath("NativeAdrenoMetal.publish", path, sizeof path);
}
static Gpu *makeGpu(unsigned port) {
    void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL);
    if (!iokit) return nullptr;
    Gpu *gpu = new Gpu;
    gpu->call = (CallFn)dlsym(iokit, "IOConnectCallStructMethod");
    gpu->map = (MapFn)dlsym(iokit, "IOConnectMapMemory64");
    gpu->unmap = (UnmapFn)dlsym(iokit, "IOConnectUnmapMemory64");
    gpu->openFn = (IOServiceOpenFn)dlsym(iokit, "IOServiceOpen");
    gpu->closeFn = (IOServiceCloseFn)dlsym(iokit, "IOServiceClose");
    gpu->port = port;
    return gpu;
}
static std::string gCacheDirectory;   // the runtime's cache directory (the one that opened the device), exported by CacheExport.h
// Progress markers 0x68xx (GPU6): 6800 entered, 6801 Turnip found, 6802 device made, 6803 returned to the discovery, 68f1 no Turnip, 68f2 no device.
static unsigned gDiagBits;
// Reads of the render server's own state for the framebuffer-format question (diagnostic only, for the exact QuartzCore of the target image; a read
// that fails or a build that differs reports zeros). Each value is one marker, 0x7Kvv: K picks the item, vv the value.
static bool readTask(uintptr_t address, void *out, size_t size) {
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), address, size, (vm_address_t) out, &got) == KERN_SUCCESS && got == size;
}
static bool writeTask(uintptr_t address, const void *data, size_t size) {
    return vm_write(mach_task_self(), address, (vm_offset_t) data, (mach_msg_type_number_t) size) == KERN_SUCCESS;
}
// EXPERIMENT (second report onward): this display path takes plain 8-bit BGRA only, so tell the render server's display that it has neither the deep
// (10-bit) framebuffer nor universal buffer compression. Whether the state it reads those from is the whole story is what the markers show.
static unsigned gReports;
struct DisplaySample {unsigned flags = 0, mode = 0, behaviour = 0, globals = 0; uint16_t display = 0; uintptr_t object = 0;};
static DisplaySample sampleDisplay(id first, intptr_t slide) {
    DisplaySample out;
    uintptr_t a = first ? (uintptr_t) *(void **) ((char *) (__bridge void *) first + 8) : 0, b = 0, d = 0;
    if (a && readTask(a + 0x40, &b, 8) && b && readTask(b + 0x60, &d, 8) && d) {
        uint32_t f = 0; uint64_t m = 0; uint8_t byte[4] = {};
        readTask(d + 0x238, &f, 4);
        readTask(d + 0x258, &out.display, 2);
        readTask(d + 0x248, &m, 8);
        readTask(d + 0x6c17, &byte[0], 1);
        readTask(d + 0x6658 + 0x57d, &byte[1], 1);
        readTask(d + 0x6658 + 0x58a, &byte[2], 1);
        readTask(d + 0x6658 + 0x59b, &byte[3], 1);
        out.object = d;
        out.flags = f & 0xff;
        out.mode = (unsigned) ((m >> 55) & 31) | (m ? 0x20 : 0);
        out.behaviour = (byte[0] ? 1 : 0) | (byte[1] ? 2 : 0) | (byte[2] ? 4 : 0) | (byte[3] ? 8 : 0);
    }
    uint8_t v[4] = {};
    readTask(0x1f491ae10 + slide, &v[0], 1);    // the render server's deep-framebuffer flag
    readTask(0x1ed7abf10 + slide, &v[1], 1);    // framebuffer compression in effect
    readTask(0x1f4919dc0 + slide, &v[2], 1);    // its default
    readTask(0x1f4919c20 + slide, &v[3], 1);    // universal buffer compression
    out.globals = (v[0] ? 1 : 0) | (v[1] ? 2 : 0) | (v[2] ? 4 : 0) | (v[3] ? 8 : 0);
    return out;
}
static void markSample(Gpu &gpu, const DisplaySample &x, unsigned base) {
    gpu.mark(base | (x.flags << 4) | x.globals);                  // display flags (bit 3 deep), globals: deep/compression/default/universal
    gpu.mark((base + 0x1000) | (x.display >> 4));                 // display limits word: 0x200 compression support, 0x1c00 type
    gpu.mark((base + 0x2000) | (x.mode << 4) | x.behaviour);     // mode colour field, ICC/other behaviour bytes
}
static void reportDisplayState(Gpu &gpu) {
    if (gReports >= 0) return;   // data-a-live: the addresses below are iPadOS 17.7.11 QuartzCore's, not iOS 16's
    ++gReports;
    static DisplaySample before;
    static bool changed;
    Class serverClass = NSClassFromString(@"CAWindowServer");
    void *start = dlsym(RTLD_DEFAULT, "CARenderServerStart");
    if (!serverClass || !start) return;
    const intptr_t slide = (intptr_t) start - 0x189651efc;
    @try {
        id server = ((id (*)(id, SEL)) objc_msgSend)(serverClass, NSSelectorFromString(@"serverIfRunning"));
        NSArray *displays = server ? ((id (*)(id, SEL)) objc_msgSend)(server, NSSelectorFromString(@"displays")) : nil;
        id first = displays.count ? displays[0] : nil;
        DisplaySample now = sampleDisplay(first, slide);
        if (!changed) {
            before = now;
            if (gReports >= 2 && now.object) {
                uint32_t f = 0;
                if (readTask(now.object + 0x238, &f, 4) && (f & 8)) {f &= ~8u; writeTask(now.object + 0x238, &f, 4);}
                uint8_t zero = 0;
                writeTask(0x1f4919c20 + slide, &zero, 1);
                changed = true;
                now = sampleDisplay(first, slide);
            }
        }
        markSample(gpu, before, 0x7000);
        markSample(gpu, now, 0xA000);
    } @catch (NSException *) {}
}
// Whether NativeAdrenoMetal.clients names this process (names separated by blanks, commas or new lines; "*" names every process).
static bool listedClient() {
    char path[1024];
    if (!requestedPath("NativeAdrenoMetal.clients", path, sizeof path)) return false;
    FILE *file = fopen(path, "r");
    if (!file) return false;
    char text[1024] = {};
    const size_t length = fread(text, 1, sizeof text - 1, file);
    fclose(file);
    text[length] = 0;
    const char *self = getprogname();
    for (char *save = nullptr, *name = strtok_r(text, " ,\t\r\n", &save); name; name = strtok_r(nullptr, " ,\t\r\n", &save))
        if (!strcmp(name, "*") || !strcmp(name, self)) return true;
    return false;
}
// A listed process other than the render server: the same runtime on its own connection, with none of the render server's threads (no
// display hand-off, telemetry, benchmark or cache export). Its shader cache lives in its own cache or temp directory.
// The reason a client's device failed, as a text line the driver keeps per process (the render server's report row is rewritten every ten seconds):
// "<step> <detail>", through the marker stream (a connection is not needed). The Gpu object is made only on this path.
static void clientFailure(unsigned port, const char *step, const char *detail) {
    // dada: the kernel's accelerator service is not the native driver's, so no marker stream: the console and the log
    (void)port;
    char text[256];
    const int n = snprintf(text, sizeof text, "gpu2-metal[%s %d]: no device: %s %s\n", getprogname(), getpid(), step, detail ? detail : "");
    if (int fd = open("/dev/console", O_WRONLY); fd >= 0) {(void)write(fd, text, (size_t)n); close(fd);}
    NSLog(@"%s", text);
}
static id clientDevice(unsigned port) {
    if (!listedClient()) return nil;
    // dada: every process's device is made here (initWithAcceleratorPort: comes straight to clientDevice; deviceForPort only serves
    // the publish mode), so the environment Turnip and the runtime read is set here: Turnip's own errors to the console; 8 MiB memory
    // blocks for the small sandboxed clients (the kernel refuses them 64 MiB: g2vk20/21 posters and widgets, BoAlloc 67108864
    // 0xe00002bd), 64 for the render server and SpringBoard; a plain-row framebuffer for the render server.
    setenv("MESA_LOG_FILE", "/dev/console", 0);
    // 32 MiB for the render server and SpringBoard (a full-screen texture, 12.6 MB, still fits a block twice), 8 for the rest: the GPU's
    // address space is one 3.5 GB range for every process (RedwoodGPU), and 64 MiB blocks helped fill it (g2vk22)
    const bool big = !strcmp(getprogname(), "backboardd") || !strcmp(getprogname(), "SpringBoard");
    setenv("M2V_BLOCK_MB", big ? "16" : "8", 0);   // 16: less held in half-used blocks (g2k4: 17 blocks of 32 MiB)
    if (!strcmp(getprogname(), "backboardd")) setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
    // dada: the posters (wallpapers) single-sampled (NativeBridgeMetalFrontend.mm gSingleSample: their 4x MSAA frames held the one ring
    // 135-195 ms each, g2k31); the plug-in's own process has the variable before the frontend's statics read it only if set here first
    if (strstr(getprogname(), "Poster")) {
        setenv("NB_SINGLE_SAMPLE", "1", 0);
        setenv("NB_MIN_COMMIT_MS", "100", 0);   // (NativeBridgeMetalFrontend.mm minCommitUs: their frames held the ring 40-55 ms, g2k32)
    }
    // dada: Turnip's compiled shaders on disk, inside the runtime's cache directory (exported with it: tools/m2vpack.py); set per
    // directory below

    char turnip[1024];
    if (!turnip::libraryPath(turnip, sizeof turnip) || access(turnip, R_OK) != 0) {clientFailure(port, "no-turnip", ""); return nil;}
    uint64_t registryID = 0;
    if (void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL)) {
        if (auto retain = (int (*)(unsigned))dlsym(iokit, "IOObjectRetain")) retain(port);
        if (auto entryID = (int (*)(unsigned, uint64_t *))dlsym(iokit, "IORegistryEntryGetRegistryEntryID")) entryID(port, &registryID);
    }
    // IOSurfaces this GPU reads or writes must be plain rows, as in the render server.
    CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
    // dada: one cache directory for every process of the user (mobile): the render server, SpringBoard, the wallpapers, the widgets
    // and the applications compile the same CoreAnimation pipelines, and each compiled all of them again in a directory of its own
    // (g2k34: 213 pipeline makings in backboardd alone, 7.7 s, on all eight cores busy at boot with the posters' and widgets' own).
    // The runtime's entries and Mesa's are written whole and checked when read, so processes can share one. The image's pack goes
    // in once a boot, by whoever comes first (the "imported" mark); the export (backboardd) then carries everybody's pipelines.
    // A process that cannot write there (another user, a sandbox) uses its own directories, as before. NB_SHARED_CACHE=0: never.
    char candidates[3][512] = {};
    const char *shared = "/private/var/mobile/Library/Caches/com.redwood.m2v-shared/";
    if (getuid() == 501 && !(getenv("NB_SHARED_CACHE") && !strcmp(getenv("NB_SHARED_CACHE"), "0"))) {
        mkdir("/private/var/mobile/Library/Caches", 0755);
        mkdir(shared, 0700);
        if (access(shared, W_OK) == 0) strlcpy(candidates[0], shared, sizeof candidates[0] - 16);
    }
    if (!confstr(_CS_DARWIN_USER_CACHE_DIR, candidates[1], sizeof candidates[1] - 16)) candidates[1][0] = 0;
    if (!confstr(_CS_DARWIN_USER_TEMP_DIR, candidates[2], sizeof candidates[2] - 16)) candidates[2][0] = 0;
    id device = nil;
    const char *lastError = "";
    unsigned usable = 0;
    for (unsigned i = 0; i < 3 && !device; ++i) {
        if (!candidates[i][0]) continue;
        strlcat(candidates[i], "m2v", sizeof candidates[i]);
        mkdir(candidates[i], 0700);
        if (access(candidates[i], W_OK) != 0) continue;
        ++usable;
        {
            std::string mesa = std::string(candidates[i]) + "/mesa";
            mkdir(mesa.c_str(), 0700);
            setenv("MESA_SHADER_CACHE_DIR", mesa.c_str(), 1);
            setenv("MESA_SHADER_CACHE_MAX_SIZE", "256M", 0);
        }
        // dada: the image's pipeline pack (earlier boots' exports, ~/hakios-gpu2/tools/m2vpack.py) into this cache directory before the
        // runtime reads it, for the render server and SpringBoard (deviceForPort did it, which no process takes any more)
        // (into the shared directory once a boot, by the first process there; into a directory of its own, for the render server and
        // SpringBoard as before)
        bool first = big;
        if (i == 0) {
            const std::string mark = std::string(candidates[i]) + "/imported";
            const int fd = open(mark.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
            first = fd >= 0;
            if (fd >= 0) close(fd);
        }
        if (first)
            if (char pack[1024]; requestedPath("NativeAdrenoMetal.m2vpack", pack, sizeof pack)) {
                const int imported = cacheexport::importPack(pack, candidates[i]);
                NSLog(@"gpu2-metal: pipeline pack: %d files into %s", imported, candidates[i]);
            }
        @try {
            device = native_bridge_metal_open(turnip, candidates[i], registryID, nullptr);
        } @catch (NSException *exception) {
            device = nil;
            lastError = "exception";
        }
        if (!device && !lastError[0]) lastError = nmb_last_error();
        if (device) gCacheDirectory = candidates[i];
    }
    // dada: Turnip names the IOSurfaces it is asked to import through this (RedwoodGPU imports a surface by its ID)
    if (device) {
        if (void *library = dlopen(turnip, RTLD_NOW | RTLD_NOLOAD))
            native_bridge_metal_set_surface_note((void (*)(uint32_t, const void *, uint64_t))dlsym(library, "tu_darwin_note_surface"));
        if (void *library = dlopen(turnip, RTLD_NOW | RTLD_NOLOAD))
            native_bridge_metal_set_lost_reason((const char *(*)(void))dlsym(library, "tu_darwin_lost_reason"));
    }
    if (!device) clientFailure(port, usable ? "no-device" : "no-cachedir", lastError);
    else if (!strcmp(getprogname(), "backboardd")) {
        // data-a-live: the render server as a client (no publish mode here): what it reads for the deep
        // framebuffer, and the bridge's telemetry (refused surfaces, last problems) every 30 s.
        CFTypeRef deep = CFPreferencesCopyAppValue(CFSTR("CAEnableDeepFramebuffer"), kCFPreferencesCurrentApplication);
        NSLog(@"pandora-metal: backboardd has a device; CAEnableDeepFramebuffer %@", (__bridge id)deep);
        pthread_t timing;   // data-a-live: the bridge's time and Turnip's render passes, every 10 s for 10 minutes
        if (pthread_create(&timing, nullptr, [](void *) -> void * {
                static char text[4096], work[8192];
                native_bridge_metal_pandora_timing(text, sizeof text);   // starts the first interval
                for (unsigned round = 0; round < 60; ++round) {
                    sleep(10);
                    native_bridge_metal_pandora_timing(text, sizeof text);
                    for (char *line = text, *end; *line; line = end) {
                        end = strchr(line, '\n');
                        if (end) *end++ = 0; else end = line + strlen(line);
                        NSLog(@"pandora-metal B%u %s", round, line);
                    }
                    // The runtime's work counts that changed (round 0: since it started): shadow copies and the like.
                    native_bridge_metal_pandora_work(work, sizeof work);
                    for (char *line = work, *end; *line; line = end) {
                        end = strchr(line, '\n');
                        if (end) *end++ = 0; else end = line + strlen(line);
                        NSLog(@"pandora-metal W%u %s", round, line);
                    }
                }
                return nullptr;
            }, nullptr) == 0) pthread_detach(timing);
        if (deep) CFRelease(deep);
        // dada: the pipeline-cache export (RedwoodGPU's tunnel, boot-arg gpu_export_mb), which only deviceForPort started before --
        // not while the image has the job that sends /private/var as a whole (the shared shader cache with it): the tunnel takes one
        // sender, and two would overwrite each other's pieces (userland/src/stateexport.c)
        const bool stateExport = access("/redwood/LaunchDaemons/com.redwood.stateexport.plist", F_OK) == 0;
        if (Gpu *gpu = stateExport ? nullptr : makeGpu(port)) {
            pthread_t exporter;
            if (pthread_create(&exporter, nullptr, [](void *arg) -> void * {
                    cacheexport::run(*static_cast<Gpu *>(arg), gCacheDirectory);
                    return nullptr;
                }, gpu) == 0) pthread_detach(exporter);
        }
        pthread_t thread;
        if (pthread_create(&thread, nullptr, [](void *) -> void * {
                static char table[6144];
                for (unsigned round = 0; round < 10; ++round) {
                    sleep(30);
                    native_bridge_metal_telemetry(table, sizeof table);
                    for (char *line = table, *end; *line; line = end) {
                        end = strchr(line, '\n');
                        if (end) *end++ = 0; else end = line + strlen(line);
                        NSLog(@"pandora-metal T%u %s", round, line);
                    }
                }
                return nullptr;
            }, nullptr) == 0) pthread_detach(thread);
    }
    return device;
}
static id deviceForPort(unsigned port) {
    static id device;
    static bool tried;
    @synchronized([NativeAdrenoMetalDeviceBase class]) {
        if (tried) return device;
        tried = true;
        exceptiondiag::start(); // the throw site of an uncaught exception, for the kernel's THROW row (ExceptionDiag.h)
        // Every Metal process renders in system memory: Turnip's GMEM passes hand work between the BV and BR threads, and with IFPC off
        // and a context reset on every client switch the two still stopped waiting on each other inside a pass (int-261p, int-262: GPU idle,
        // BR one IB ahead, BV inside a draw IB2). int-257's nobin covered the render server only. System memory alone still let BV run
        // ahead of BR across passes (concurrent binning is emitted for sysmem passes too) and it hung the same way in int-265: no
        // concurrent binning either.
        setenv("TU_DEBUG", "sysmem,nocb", 0);
        setenv("MESA_LOG_FILE", "/dev/console", 0);
        // dada: 8 MiB memory blocks for the small sandboxed clients (the kernel refuses them 64 MiB), the default 64 for the render server
        // and SpringBoard, whose full-screen textures would otherwise each be an allocation of their own
        if (strcmp(getprogname(), "backboardd") != 0 && strcmp(getprogname(), "SpringBoard") != 0) setenv("M2V_BLOCK_MB", "8", 0);   // dada: Turnip's own errors (device lost, refusals) where a run keeps them
        // dada: the kernel has no native-driver marker ABI (makeGpu/mark are the A740 driver's), so the render server opens its device
        // like any client, over Turnip's RedwoodGPU transport, with a plain-row framebuffer.
        if (!strcmp(getprogname(), "backboardd")) setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
        if (getenv("GPU2_NATIVE_BACKBOARDD") == nullptr || strcmp(getprogname(), "backboardd") != 0) return device = clientDevice(port);
        // The discovery releases the accelerator port when it is done; the monitor and the scanout client keep using it.
        if (void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL)) {
            if (auto retain = (int (*)(unsigned))dlsym(iokit, "IOObjectRetain")) retain(port);
        }
        Gpu *gpu = makeGpu(port);
        if (!gpu) return nil;
        gpu->mark(0x6800);
        char turnip[1024];
        if (!turnip::libraryPath(turnip, sizeof turnip) || access(turnip, R_OK) != 0) {gpu->mark(0x68f1); return nil;}
        gpu->mark(0x6801);
        // The runtime refuses to open without a writable shader-cache directory, and which directories backboardd's sandbox lets it write is not
        // known from here: try the user cache and temp directories, then /tmp and /var/tmp, and use the first that opens a device.
        uint64_t registryID = 0;
        if (void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL)) {
            if (auto entryID = (int (*)(unsigned, uint64_t *))dlsym(iokit, "IORegistryEntryGetRegistryEntryID")) entryID(port, &registryID);
        }
        // What the render server sees for its framebuffer choice (marker 0x6Axx): bit0 the deep-framebuffer preference is set, bit1 it is false,
        // bit2 MobileGestalt's answer behind the render server's deep flag is true, bit3 it exists, bit4 the compression capability dictionary exists,
        // bit5 the process is not root.
        {
            unsigned bits = 0;
            if (CFTypeRef value = CFPreferencesCopyAppValue(CFSTR("CAEnableDeepFramebuffer"), kCFPreferencesCurrentApplication)) {
                bits |= 1;
                if (CFGetTypeID(value) == CFBooleanGetTypeID() && !CFBooleanGetValue((CFBooleanRef) value)) bits |= 2;
                CFRelease(value);
            }
            if (auto copyAnswer = (CFTypeRef (*)(CFStringRef)) dlsym(RTLD_DEFAULT, "MGCopyAnswer")) {
                if (CFTypeRef deep = copyAnswer(CFSTR("Aixt/MEN2O2B7f+8m4TxUA"))) {
                    bits |= 8;
                    if (CFGetTypeID(deep) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef) deep)) bits |= 4;
                    CFRelease(deep);
                }
                if (CFTypeRef caps = copyAnswer(CFSTR("AoKnINTLPoKML3ctoP0AZg"))) {bits |= 16; CFRelease(caps);}
            }
            if (getuid() != 0) bits |= 32;
            gDiagBits = bits;
            gpu->mark(0x6A00 | bits);
        }
        // The render server compresses its framebuffer (an Apple/AGX layout this GPU does not write) unless told not to; the display driver takes plain rows.
        setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
        // The runtime's creation log reports each VkPipeline making's time to the device's sink, which the telemetry's V row sums.
        setenv("METAL2VULKAN_LOG_PIPELINES", "1", 0);
        CFPreferencesSetAppValue(CFSTR("CAEnableFramebufferCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
        CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
        char candidates[5][512] = {};
        if (!confstr(_CS_DARWIN_USER_CACHE_DIR, candidates[0], sizeof candidates[0] - 16)) candidates[0][0] = 0;
        if (!confstr(_CS_DARWIN_USER_TEMP_DIR, candidates[1], sizeof candidates[1] - 16)) candidates[1][0] = 0;
        strlcpy(candidates[2], "/tmp/", sizeof candidates[2]);
        strlcpy(candidates[3], "/private/var/tmp/", sizeof candidates[3]);
        // The user temp directory of the mobile user is under /var/mobile/tmp, which launchd's boot task dirs_cleaner empties at every boot,
        // so with a persistent Data store the pipelines were still compiled again each boot (V row hit0). Try the user's Library/Caches first
        // (marker 0x6814); the earlier candidates keep their markers.
        if (const passwd *user = getpwuid(getuid()); user && user->pw_dir && user->pw_dir[0]) {
            snprintf(candidates[4], sizeof candidates[4] - 16, "%s/Library/Caches/com.apple.backboardd", user->pw_dir);
            mkdir(candidates[4], 0700);
            strlcat(candidates[4], "/", sizeof candidates[4]);
        }
        static const unsigned order[] = {4, 0, 1, 2, 3};
        for (unsigned i : order) {
            if (device) break;
            if (!candidates[i][0]) continue;
            strlcat(candidates[i], "m2v", sizeof candidates[i]);
            mkdir(candidates[i], 0700);
            if (access(candidates[i], W_OK) != 0) continue;
            // The image's pipeline pack (an earlier boot's export), into this directory before the runtime reads it. Marker 0x6Cnn: files written
            // (saturated at 0xfe), 0x6Cff the pack was refused.
            if (char pack[1024]; requestedPath("NativeAdrenoMetal.m2vpack", pack, sizeof pack)) {
                const int imported = cacheexport::importPack(pack, candidates[i]);
                if (imported) gpu->mark(0x6C00 | (imported < 0 ? 0xff : imported > 0xfe ? 0xfe : imported));
            }
            @try {
                device = native_bridge_metal_open(turnip, candidates[i], registryID, nullptr);
            } @catch (NSException *exception) {
                device = nil;
            }
            if (device) {gpu->mark(0x6810 + i); gCacheDirectory = candidates[i];}   // which directory served
        }
        if (!device) {
            gpu->mark(0x68f2);
            char text[64];
            snprintf(text, sizeof text, "PUB no device: %.40s", nmb_last_error());
            streamText(*gpu, text);
            return nil;
        }
        gpu->mark(0x6802);
        // The status line (GPU8) every ten seconds, and the display hand-off client on its own thread: both only read or serve, neither is the discovery's.
        pthread_t monitor, scanout;
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attributes, 1u << 20);
        pthread_create(&monitor, &attributes, [](void *arg) -> void * {
            Gpu *g = static_cast<Gpu *>(arg);
            for (;;) {
                sleep(10);
                char status[64], text[64];
                static unsigned phase;
                // status (counters, then the last two refusals' texts), formats; the stream carries 63 characters per line, so a long status goes
                // out in 57-character pieces on consecutive lines (the UFS log keeps every line).
                char full[420];
                native_bridge_metal_status(full, sizeof full);
                const size_t length = strlen(full);
                const unsigned pieces = (unsigned)((length + 56) / 57);
                const unsigned step = phase++ % (1 + (pieces ? (pieces > 7 ? 7 : pieces) : 1));
                if (step == 0) {
                    native_bridge_metal_formats(status, sizeof status);
                    snprintf(text, sizeof text, "PUB %.58s", status);
                } else {
                    snprintf(text, sizeof text, "ST%u %.57s", step - 1, full + (size_t)(step - 1) * 57);
                }
                streamText(*g, text);
                reportDisplayState(*g);
                g->mark(0x6A00 | gDiagBits);   // the diagnostics page lists the last few markers only
            }
            return nullptr;
        }, gpu);
        pthread_t telemetry;
        pthread_create(&telemetry, &attributes, [](void *arg) -> void * {
            Gpu *g = static_cast<Gpu *>(arg);
            sleep(5);
            // The bridge's telemetry table, resent whole every four seconds as markers (two characters each): 0xC000|row starts a row of the
            // kernel's table, 0x8000|(a<<7)|b appends two characters. Rows are at most 90 characters; 48 rows.
            for (;;) {
                static char table[6144];
                native_bridge_metal_telemetry(table, sizeof table);
                unsigned row = 0;
                for (char *line = table; *line && row < 48;) {
                    char *end = strchr(line, '\n');
                    if (end) *end = 0;
                    g->mark(0xC000u | row);
                    const size_t length = strlen(line) < 90 ? strlen(line) : 90;
                    for (size_t i = 0; i < length; i += 2) {
                        const unsigned a = (unsigned char)line[i] & 0x7f, b = i + 1 < length ? (unsigned char)line[i + 1] & 0x7f : 0;
                        g->mark(0x8000u | (a << 7) | b);
                    }
                    ++row;
                    if (!end) break;
                    line = end + 1;
                }
                sleep(6);
            }
            return nullptr;
        }, gpu);
        pthread_create(&scanout, &attributes, [](void *arg) -> void * {
            Gpu *g = static_cast<Gpu *>(arg);
            sleep(3);
            @try {
                bridgephase::serveScanout(*g, device);
            } @catch (NSException *exception) {
                g->mark(0x68f3);
            }
            return nullptr;
        }, gpu);
        pthread_t exporter;
        pthread_create(&exporter, &attributes, [](void *arg) -> void * {
            cacheexport::run(*static_cast<Gpu *>(arg), gCacheDirectory);
            return nullptr;
        }, gpu);
        // Every Metal library of the OS through the runtime, 90 s after the render server opened the device (PrimeShaders.h); the boots that froze
        // or crashed before three minutes never primed anything. Progress is kept in the Data cache directory, so later boots resume.
        // The translator recurses deeply on large functions: give it the stack of a main thread.
        pthread_attr_setstacksize(&attributes, 8u << 20);
        pthread_t primer;
        pthread_create(&primer, &attributes, [](void *) -> void * {
            char list[1024];
            prime::run((id<MTLDevice>)device, gCacheDirectory, 90,
                       requestedPath("NativeAdrenoMetal.primelist", list, sizeof list) ? std::string(list) : std::string());
            return nullptr;
        }, nullptr);
        pthread_attr_destroy(&attributes);
        gpu->mark(0x6803);
        return device;
    }
}
}  // namespace publish

@implementation NativeAdrenoMetalDevice
- (id)initWithAcceleratorPort:(unsigned)port {
    self = [super init];
    if (!self || !port) return nil;
    if (publish::requested()) return publish::deviceForPort(port);
    // data-a-live (iOS 16, Adreno 840): no stage-1 self-test, it drives the A740's raw ABI. A process named in
    // NativeAdrenoMetal.clients gets its own device, every other process (backboardd too) nil and its software renderer.
    {   // data-a-live: TU_DEBUG from the boot-arg pandora_tu_debug=a,b,c when given (bb37), else nocb; a value
        // already in the environment wins either way (mtltest picks its own per launch). Without sysmem Turnip
        // chooses per render pass between on-chip tiles (GMEM) and memory (its autotune), its default on other
        // Adrenos; nocb keeps concurrent binning off (gen8 BV/BR stalls seen elsewhere, not yet understood here).
        static char chosen[128];
        const char *flags = "sysmem,nocb,startup"; // OP13: keep the settings verified by the standalone readback test.
        char args[1024] = {};
        size_t size = sizeof args - 1;
        if (sysctlbyname("kern.bootargs", args, &size, nullptr, 0) == 0)
            if (const char *at = strstr(args, "pandora_tu_debug=")) {
                snprintf(chosen, sizeof chosen, "%.*s", (int)strcspn(at + 17, " "), at + 17);
                flags = chosen;
            }
        setenv("TU_DEBUG", flags, 0);
        static bool told;
        if (!told) {
            told = true;
            NSLog(@"pandora-metal TU_DEBUG=%s", getenv("TU_DEBUG"));
        }
    }
    if (port) return publish::clientDevice(port);
    static bool attempted;
    if (attempted) return nil;
    attempted = true;
    // The test runs on its own thread: a kernel call that never returns (int-042: BoFree) then stalls that thread only, not SpringBoard's
    // main thread (the UI froze with the test running inline).
    // The discovery code releases the accelerator port as soon as this returns nil, which is before the test thread has started: the first
    // marker (and every later open) then went to a dead name and never reached the kernel (K0 in int-043..052, with the plugin file opened by
    // SpringBoard). Keep one extra user reference on the port, and let the thread look the service up itself as well.
    if (void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL)) {
        if (auto retain = (int (*)(unsigned))dlsym(iokit, "IOObjectRetain")) retain(port);
    }
    struct Job {unsigned port; char name[32];};
    Job *job = new Job{port, {}};
    strlcpy(job->name, getprogname(), sizeof job->name);
    pthread_t thread;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attributes, [](void *arg) -> void * {
            Job *j = static_cast<Job *>(arg);
            unsigned port = j->port;
            if (void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL)) {
                auto matching = (void *(*)(const char *))dlsym(iokit, "IOServiceMatching");
                auto getService = (unsigned (*)(unsigned, void *))dlsym(iokit, "IOServiceGetMatchingService");
                if (matching && getService) {
                    const unsigned own = getService(0, matching("IOAcceleratorES"));
                    if (own) port = own;
                }
            }
            gAfterTurnip = bridgephase::run;
            adrenoUserTest(port, j->name);
            delete j;
            return nullptr;
        }, job) != 0) delete job;
    pthread_attr_destroy(&attributes);
    return nil;
}
@end
