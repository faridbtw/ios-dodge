/*
 * dada (Xiaomi 15, Adreno 830, iOS 26): the NativeAdreno ABI carried over the kernel's RedwoodGPU service, which speaks Linux's msm DRM
 * interface as IOKit calls (selector = the ioctl's low byte; the structures as Linux's; a buffer's CPU mapping is the client memory of
 * the type that is its handle; selector 0x7e imports an IOSurface by its ID). The kernel the phone runs has RedwoodGPU and not the
 * NativeAdreno service, so this is how Turnip reaches the GPU there. Included by tu_knl_darwin.cc only.
 *
 * Differences from the native service, and how each is met:
 *  - addresses: RedwoodGPU places every buffer itself (msm 1.7 "softpin"); the backend takes that address (rw_bo_iova) instead of
 *    choosing one, and BoMap only checks that the address asked for is that one;
 *  - host imports: the kernel imports an IOSurface by ID, not a range of the caller's pages. The Metal plugin names each surface it
 *    imports (tu_darwin_note_surface) and an import of a named surface's pixels becomes the surface's; anything else is refused;
 *  - metadata and queues are this process's own (one ring, one client per process).
 */
#include <fcntl.h>
#include <stdarg.h>
#include <time.h>
#include "drm-uapi/msm_drm.h"

#define RW_SERVICE "RedwoodGPU"
#define RW_SEL(nr) (0x40u + (nr))           /* DRM_COMMAND_BASE + DRM_MSM_x */
#define RW_GEM_CLOSE 0x09u                  /* DRM_IOCTL_GEM_CLOSE */
#define RW_SURFACE 0x7eu
#define RW_HOST 0x7cu                       /* RedwoodGPU's import of the caller's own pages (kernels from 29 Oct on) */

struct rw_host_import {                     /* RedwoodGPU.cpp: struct redwood_host */
   uint64_t address, bytes;
   uint32_t handle;                         /* out */
   uint32_t flags;
};

struct rw_surface_import {                  /* tools/mesa/redwood-drm/xf86drm.h: struct redwood_drm_surface */
   uint32_t surface;                        /* the IOSurface's ID */
   uint32_t handle;                         /* out: the buffer, 1-based */
   uint64_t size;                           /* out: whole pages */
   uint32_t offset, row_bytes;              /* out */
   uint32_t width, height;                  /* out */
   uint32_t pixel_format, planes;           /* out */
};

struct rw_bo {
   uint32_t handle;
   uint64_t iova, size;
   char metadata[NativeAdrenoAbiMetadataBytes];
   uint32_t metadata_bytes;
   bool surface;                            /* an IOSurface's import (selector 0x7e), not a GEM_NEW buffer */
   bool host;                               /* the caller's own pages (RW_HOST): named in every submission, for the kernel's cache pushes */
   uint32_t refs;                           /* the backend's BOs on this kernel handle: RedwoodGPU gives a surface this client
                                             * has imported already the same handle again, and one GEM_CLOSE ends it for all */
};

static struct {
   pthread_mutex_t lock;
   struct rw_bo *bos;                       /* by handle */
   uint32_t capacity;
   struct { uint32_t id; uint64_t base, bytes; } surfaces[256];
   unsigned surface_next;
   uint32_t queue;                          /* the msm submit queue (0: the default one) */
   uint32_t hosts[512];                     /* the live RW_HOST handles */
   unsigned host_count;
   /*
    * dada: the live IOSurface imports, also named in every submission. RedwoodGPU marks a surface "drawn" by the last
    * submission that names it, and the display's driver waits for that one before it copies the surface to the panel
    * (MiniIOSurfaceDrawn). Only the command buffers were named, so no surface was ever "drawn": the window server's
    * frames went to the panel while the GPU was still drawing them -- the wallpaper drawn and the interface not yet, or
    * the top of a frame new and the bottom old (dada g2k37s/g2k38s recordings: the flicker and tearing of every swipe).
    */
   uint32_t surfs[256];
   unsigned surf_count;
} rw = { .lock = PTHREAD_MUTEX_INITIALIZER };

static kern_return_t
rw_raw(uint32_t selector, void *data, size_t size)
{
   size_t returned = size;
   return IOConnectCallStructMethod(dw.conn, selector, data, size, data, &returned);
}

static uint64_t
rw_param(uint32_t param, uint64_t fallback)
{
   struct drm_msm_param p = {.pipe = MSM_PIPE_3D0, .param = param};
   return rw_raw(RW_SEL(DRM_MSM_GET_PARAM), &p, sizeof(p)) == KERN_SUCCESS ? p.value : fallback;
}

static struct rw_bo *
rw_bo_slot(uint32_t handle, bool grow)
{
   if (handle >= rw.capacity) {
      if (!grow)
         return NULL;
      uint32_t capacity = MAX2(rw.capacity * 2, MAX2(handle + 1, 1024u));
      struct rw_bo *bos = (struct rw_bo *)realloc(rw.bos, capacity * sizeof(*bos));
      if (!bos)
         return NULL;
      memset(bos + rw.capacity, 0, (capacity - rw.capacity) * sizeof(*bos));
      rw.bos = bos;
      rw.capacity = capacity;
   }
   return &rw.bos[handle];
}

static uint64_t
rw_bo_iova(uint64_t handle)
{
   pthread_mutex_lock(&rw.lock);
   struct rw_bo *b = rw_bo_slot((uint32_t)handle, false);
   uint64_t iova = b && b->handle ? b->iova : 0;
   pthread_mutex_unlock(&rw.lock);
   return iova;
}

/* A buffer the kernel made: its address, kept by handle. */
static kern_return_t
rw_note_bo(uint32_t handle, uint64_t size)
{
   struct drm_msm_gem_info info = {.handle = handle, .info = MSM_INFO_GET_IOVA};
   kern_return_t kr = rw_raw(RW_SEL(DRM_MSM_GEM_INFO), &info, sizeof(info));
   if (kr != KERN_SUCCESS || !info.value)
      return kr ? kr : kIOReturnError;
   pthread_mutex_lock(&rw.lock);
   struct rw_bo *b = rw_bo_slot(handle, true);
   if (b && b->handle == handle && b->refs)
      b->refs++;                            /* a surface imported again: the same kernel buffer */
   else if (b)
      *b = (struct rw_bo){.handle = handle, .iova = info.value, .size = size, .refs = 1};
   pthread_mutex_unlock(&rw.lock);
   return b ? KERN_SUCCESS : kIOReturnNoMemory;
}

/* Exported for the Metal plugin: the IOSurface whose pixels are [base, base + bytes), as it imports them. */
extern "C" __attribute__((visibility("default"))) void tu_darwin_note_surface(uint32_t id, const void *base, uint64_t bytes);
extern "C" __attribute__((visibility("default"))) void
tu_darwin_note_surface(uint32_t id, const void *base, uint64_t bytes)
{
   if (!id || !base)
      return;
   pthread_mutex_lock(&rw.lock);
   unsigned free_at = rw.surface_next % ARRAY_SIZE(rw.surfaces);
   for (unsigned i = 0; i < ARRAY_SIZE(rw.surfaces); i++)
      if (rw.surfaces[i].base == (uint64_t)(uintptr_t)base) {
         free_at = i;
         break;
      }
   rw.surfaces[free_at].id = id;
   rw.surfaces[free_at].base = (uint64_t)(uintptr_t)base;
   rw.surfaces[free_at].bytes = bytes;
   if (free_at == rw.surface_next % ARRAY_SIZE(rw.surfaces))
      rw.surface_next++;
   pthread_mutex_unlock(&rw.lock);
}

/* dada: what the kernel or this transport refused, on the console (the clients' own logs do not reach it): the first 40 in a process,
 * then every 200th. */
static void PRINTFLIKE(1, 2)
rw_log(const char *format, ...)
{
   static int fd = -2;
   static unsigned count;
   unsigned n = __atomic_fetch_add(&count, 1, __ATOMIC_RELAXED);
   if (n >= 40 && n % 200)
      return;
   if (fd == -2)
      fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
   if (fd < 0)
      return;
   char line[320];
   int at = snprintf(line, sizeof(line), "gpu2-turnip[%s %d] #%u: ", getprogname(), getpid(), n);
   va_list args;
   va_start(args, format);
   at += vsnprintf(line + at, sizeof(line) - at - 1, format, args);
   va_end(args);
   if (at > (int)sizeof(line) - 2)
      at = sizeof(line) - 2;
   line[at++] = '\n';
   (void)write(fd, line, at);
}

/* dada: what this process holds of the GPU's memory, on the console every few seconds of allocating (g2k1: backboardd held 2336
 * buffers, 806 MB, and the kernel refused everyone more) */
static void
rw_census(void)
{
   static uint64_t last;
   uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
   if (now - last < 5000000000ull)
      return;
   last = now;
   unsigned n[3] = {0, 0, 0}, by_size[5] = {0, 0, 0, 0, 0};
   uint64_t bytes[3] = {0, 0, 0}, size_bytes[5] = {0, 0, 0, 0, 0};
   pthread_mutex_lock(&rw.lock);
   for (uint32_t h = 0; h < rw.capacity; h++) {
      const struct rw_bo *b = &rw.bos[h];
      if (!b->handle)
         continue;
      unsigned k = b->surface ? 1 : b->host ? 2 : 0;
      n[k]++;
      bytes[k] += b->size;
      if (k == 0) {
         unsigned z = b->size <= (64u << 10) ? 0 : b->size <= (1u << 20) ? 1 : b->size <= (8u << 20) ? 2 : b->size <= (32u << 20) ? 3 : 4;
         by_size[z]++;
         size_bytes[z] += b->size;
      }
   }
   pthread_mutex_unlock(&rw.lock);
   char line[400];
   int at = snprintf(line, sizeof(line), "gpu2-turnip[%s %d]: holds %u buffers (%llu MB: <=64K %u/%lluM, <=1M %u/%lluM, <=8M %u/%lluM, "
                     "<=32M %u/%lluM, more %u/%lluM), %u surface imports (%llu MB), %u host imports (%llu MB)\n",
                     getprogname(), getpid(), n[0], (unsigned long long)(bytes[0] >> 20), by_size[0], (unsigned long long)(size_bytes[0] >> 20),
                     by_size[1], (unsigned long long)(size_bytes[1] >> 20), by_size[2], (unsigned long long)(size_bytes[2] >> 20), by_size[3],
                     (unsigned long long)(size_bytes[3] >> 20), by_size[4], (unsigned long long)(size_bytes[4] >> 20), n[1],
                     (unsigned long long)(bytes[1] >> 20), n[2], (unsigned long long)(bytes[2] >> 20));
   int fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
   if (fd >= 0) {
      (void)write(fd, line, at);
      close(fd);
   }
}

static kern_return_t rw_call_inner(uint32_t method, const void *in, size_t in_size, void *out, size_t out_size);
/* dada: how long each kind of call into the kernel takes (all but fence waits), on the console every 10 s: g2k7's render server took
 * 5.3 ms a texture on the phone, 0.002 to 0.06 ms on the Mac */
static void
rw_time(uint32_t method, uint64_t ns)
{
   static struct {uint64_t count, ns, max, slow;} t[32];
   static uint64_t last;
   static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
   if (method >= 32)
      return;
   pthread_mutex_lock(&lock);
   t[method].count++;
   t[method].ns += ns;
   if (ns > t[method].max)
      t[method].max = ns;
   if (ns > 1000000)
      t[method].slow++;
   uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
   if (!last)
      last = now;
   if (now - last >= 10000000000ull) {
      last = now;
      char line[600];
      int at = snprintf(line, sizeof(line), "gpu2-rwtime[%s %d]:", getprogname(), getpid());
      for (unsigned m = 0; m < 32 && at < (int)sizeof(line) - 80; m++)
         if (t[m].count)
            at += snprintf(line + at, sizeof(line) - at, " m%u %llu/%llums max %lluus slow %llu;", m, (unsigned long long)t[m].count,
                           (unsigned long long)(t[m].ns / 1000000), (unsigned long long)(t[m].max / 1000), (unsigned long long)t[m].slow);
      memset(t, 0, sizeof(t));
      line[at++] = '\n';
      int fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
      if (fd >= 0) {
         (void)write(fd, line, at);
         close(fd);
      }
   }
   pthread_mutex_unlock(&lock);
}

static kern_return_t
rw_call(uint32_t method, const void *in, size_t in_size, void *out, size_t out_size)
{
   uint64_t started = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
   kern_return_t kr = rw_call_inner(method, in, in_size, out, out_size);
   if (method != NativeAdrenoMethodWaitFence)
      rw_time(method, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - started);
   if (method == NativeAdrenoMethodBoAlloc || method == NativeAdrenoMethodBoImport)
      rw_census();
   if (kr == KERN_SUCCESS || (method == NativeAdrenoMethodWaitFence && kr == kIOReturnTimeout))
      return kr;
   switch (method) {
   case NativeAdrenoMethodBoAlloc:
      rw_log("BoAlloc %llu bytes: 0x%x", (unsigned long long)((const NativeAdrenoAbiBoAlloc *)in)->bytes, kr);
      break;
   case NativeAdrenoMethodBoImport:
      rw_log("BoImport %#llx %llu bytes: 0x%x", (unsigned long long)((const NativeAdrenoAbiBoImport *)in)->address,
             (unsigned long long)((const NativeAdrenoAbiBoImport *)in)->bytes, kr);
      break;
   case NativeAdrenoMethodWaitFence:
      rw_log("WaitFence %u: 0x%x", (unsigned)((const NativeAdrenoAbiWaitFence *)in)->fence, kr);
      break;
   default:
      rw_log("method %u: 0x%x", method, kr);
   }
   return kr;
}

static kern_return_t
rw_call_inner(uint32_t method, const void *in, size_t in_size, void *out, size_t out_size)
{
   switch (method) {
   case NativeAdrenoMethodQuery: {
      NativeAdrenoAbiParams *p = (NativeAdrenoAbiParams *)out;
      if (out_size < sizeof(*p))
         return kIOReturnBadArgument;
      *p = (NativeAdrenoAbiParams){.abi_version = NativeAdrenoAbiVersion, .input_bits = 48, .output_bits = 48,
                                   .flags = NativeAdrenoParamsHostImport, .max_bo_bytes = 1ull << NativeAdrenoAbiMaxBoBytesShift};
      return KERN_SUCCESS;
   }
   case NativeAdrenoMethodGetDeviceInfo: {
      NativeAdrenoAbiDeviceInfo *d = (NativeAdrenoAbiDeviceInfo *)out;
      if (out_size < sizeof(*d))
         return kIOReturnBadArgument;
      memset(d, 0, sizeof(*d));
      d->chip_id = (uint32_t)rw_param(MSM_PARAM_CHIP_ID, 0);
      if (!d->chip_id)
         return kIOReturnNotReady;
      d->gmem_bytes = (uint32_t)rw_param(MSM_PARAM_GMEM_SIZE, 0);
      d->gmem_base = rw_param(MSM_PARAM_GMEM_BASE, 0x100000);
      d->uche_trap_base = rw_param(MSM_PARAM_UCHE_TRAP_BASE, 0x1fffffffff000ull);
      d->va_start = rw_param(MSM_PARAM_VA_START, 0x100000000ull);
      d->va_size = rw_param(MSM_PARAM_VA_SIZE, 0xfff00000000ull);
      d->highest_bank_bit = (uint32_t)rw_param(MSM_PARAM_HIGHEST_BANK_BIT, 16);
      d->ubwc_swizzle = (uint32_t)rw_param(MSM_PARAM_UBWC_SWIZZLE, 0x6);
      d->macrotile_mode = (uint32_t)rw_param(MSM_PARAM_MACROTILE_MODE, 1);
      d->ubwc_version = 0;                  /* no UBWC until the kernel says what the display reads */
      d->flags = 0;                         /* the kernel places buffers */
      return KERN_SUCCESS;
   }
   case NativeAdrenoMethodBoAlloc: {
      const NativeAdrenoAbiBoAlloc *a = (const NativeAdrenoAbiBoAlloc *)in;
      NativeAdrenoAbiBoAllocReply *r = (NativeAdrenoAbiBoAllocReply *)out;
      struct drm_msm_gem_new req = {.size = a->bytes, .flags = MSM_BO_WC};
      kern_return_t kr = rw_raw(RW_SEL(DRM_MSM_GEM_NEW), &req, sizeof(req));
      if (kr != KERN_SUCCESS)
         return kr;
      if ((kr = rw_note_bo(req.handle, a->bytes)) != KERN_SUCCESS) {
         struct drm_gem_close c = {.handle = req.handle};
         rw_raw(RW_GEM_CLOSE, &c, sizeof(c));
         return kr;
      }
      r->handle = req.handle;
      return KERN_SUCCESS;
   }
   case NativeAdrenoMethodBoImport: {
      const NativeAdrenoAbiBoImport *a = (const NativeAdrenoAbiBoImport *)in;
      NativeAdrenoAbiBoAllocReply *r = (NativeAdrenoAbiBoAllocReply *)out;
      uint32_t id = 0;
      pthread_mutex_lock(&rw.lock);
      for (unsigned i = 0; i < ARRAY_SIZE(rw.surfaces); i++)
         if (rw.surfaces[i].id && rw.surfaces[i].base == a->address && a->bytes <= rw.surfaces[i].bytes + 4095)
            id = rw.surfaces[i].id;
      pthread_mutex_unlock(&rw.lock);
      if (!id) {
         /* not an IOSurface the plugin named: the kernel imports surfaces only */
         uint64_t near_base = 0, near_bytes = 0;
         pthread_mutex_lock(&rw.lock);
         for (unsigned i = 0; i < ARRAY_SIZE(rw.surfaces); i++)
            if (rw.surfaces[i].id && rw.surfaces[i].base <= a->address && a->address < rw.surfaces[i].base + rw.surfaces[i].bytes)
               near_base = rw.surfaces[i].base, near_bytes = rw.surfaces[i].bytes;
         pthread_mutex_unlock(&rw.lock);
         /* not a surface: the caller's own pages, which RedwoodGPU imports as they are (RW_HOST) */
         struct rw_host_import host = {.address = a->address, .bytes = a->bytes};
         /* off unless TU_DARWIN_HOST_IMPORT=1: g2k2, the first run with it, froze the phone a minute in (the plugin mirrors these) */
         static int enabled = -1;
         if (enabled < 0)
            enabled = getenv("TU_DARWIN_HOST_IMPORT") && *getenv("TU_DARWIN_HOST_IMPORT") == '1';
         kern_return_t hk = enabled ? rw_raw(RW_HOST, &host, sizeof(host)) : kIOReturnUnsupported;
         if (hk == KERN_SUCCESS && (hk = rw_note_bo(host.handle, a->bytes)) == KERN_SUCCESS) {
            pthread_mutex_lock(&rw.lock);
            struct rw_bo *b = rw_bo_slot(host.handle, false);
            if (b)
               b->host = true;
            if (rw.host_count < ARRAY_SIZE(rw.hosts))
               rw.hosts[rw.host_count++] = host.handle;
            pthread_mutex_unlock(&rw.lock);
            r->handle = host.handle;
            return KERN_SUCCESS;
         }
         rw_log("import %#llx %llu bytes is no named surface (inside one at %#llx of %llu bytes: %s); RW_HOST: 0x%x",
                (unsigned long long)a->address, (unsigned long long)a->bytes, (unsigned long long)near_base,
                (unsigned long long)near_bytes, near_base ? "yes" : "no", hk);
         return kIOReturnUnsupported;
      }
      struct rw_surface_import req = {.surface = id};
      kern_return_t kr = rw_raw(RW_SURFACE, &req, sizeof(req));
      if (kr != KERN_SUCCESS) {
         rw_log("surface %u (%llu bytes asked): the kernel refused it: 0x%x", id, (unsigned long long)a->bytes, kr);
         return kr;
      }
      if ((kr = rw_note_bo(req.handle, a->bytes)) != KERN_SUCCESS)
         return kr;
      pthread_mutex_lock(&rw.lock);
      if (struct rw_bo *b = rw_bo_slot(req.handle, false)) {
         b->surface = true;
         unsigned at = 0;
         while (at < rw.surf_count && rw.surfs[at] != req.handle)
            at++;
         if (at == rw.surf_count && rw.surf_count < ARRAY_SIZE(rw.surfs))
            rw.surfs[rw.surf_count++] = req.handle;
      }
      pthread_mutex_unlock(&rw.lock);
      r->handle = req.handle;
      return KERN_SUCCESS;
   }
   case NativeAdrenoMethodBoMap: {
      const NativeAdrenoAbiBoMap *m = (const NativeAdrenoAbiBoMap *)in;
      NativeAdrenoAbiBoMapReply *r = (NativeAdrenoAbiBoMapReply *)out;
      uint64_t iova = rw_bo_iova(m->handle);
      if (!iova || (m->iova && m->iova != iova))
         return kIOReturnBadArgument;
      if (r)
         r->iova = iova;
      return KERN_SUCCESS;
   }
   case NativeAdrenoMethodBoFree: {
      const NativeAdrenoAbiBoFree *f = (const NativeAdrenoAbiBoFree *)in;
      struct drm_gem_close c = {.handle = (uint32_t)f->handle};
      pthread_mutex_lock(&rw.lock);
      struct rw_bo *b = rw_bo_slot((uint32_t)f->handle, false);
      if (b && b->refs > 1) {               /* another BO still has this kernel buffer (a surface imported twice) */
         b->refs--;
         pthread_mutex_unlock(&rw.lock);
         return KERN_SUCCESS;
      }
      bool surface = b && b->surface;
      uint64_t size = b ? b->size : 0;
      if (b && b->surface) {
         for (unsigned i = 0; i < rw.surf_count; i++)
            if (rw.surfs[i] == (uint32_t)f->handle) {
               rw.surfs[i] = rw.surfs[--rw.surf_count];
               break;
            }
      }
      if (b && b->host) {
         for (unsigned i = 0; i < rw.host_count; i++)
            if (rw.hosts[i] == (uint32_t)f->handle) {
               rw.hosts[i] = rw.hosts[--rw.host_count];
               break;
            }
      }
      if (b)
         memset(b, 0, sizeof(*b));
      pthread_mutex_unlock(&rw.lock);
      kern_return_t kr = rw_raw(RW_GEM_CLOSE, &c, sizeof(c));
      /* A refused close leaves the kernel's buffer where it was; Turnip took it for a lost device (g2vk12: "deferred BoFree failed:
       * 0xe00002c2" a minute into the run, then every frame's imports failed and the screen flickered). Logged, and the buffer
       * forgotten here. */
      if (kr != KERN_SUCCESS)
         rw_log("BoFree %u (%s, %llu bytes): the kernel refused the close: 0x%x; left to it", (unsigned)f->handle,
                surface ? "a surface's import" : "a buffer", (unsigned long long)size, kr);
      return KERN_SUCCESS;
   }
   case NativeAdrenoMethodBoCpuMap: {
      const NativeAdrenoAbiBoCpuMap *m = (const NativeAdrenoAbiBoCpuMap *)in;
      NativeAdrenoAbiBoCpuMapReply *r = (NativeAdrenoAbiBoCpuMapReply *)out;
      pthread_mutex_lock(&rw.lock);
      struct rw_bo *b = rw_bo_slot((uint32_t)m->handle, false);
      uint64_t size = b ? b->size : 0;
      pthread_mutex_unlock(&rw.lock);
      if (!size)
         return kIOReturnBadArgument;
      *r = (NativeAdrenoAbiBoCpuMapReply){.token = (uint32_t)m->handle, .bytes = size};
      return KERN_SUCCESS;
   }
   case NativeAdrenoMethodBoCpuUnmap:
      return KERN_SUCCESS;                  /* the backend's own IOConnectUnmapMemory64 undoes the mapping */
   case NativeAdrenoMethodSubmit: {
      const NativeAdrenoAbiSubmit *s = (const NativeAdrenoAbiSubmit *)in;
      NativeAdrenoAbiSubmitReply *r = (NativeAdrenoAbiSubmitReply *)out;
      if (!s->buffer_count || s->buffer_count > NativeAdrenoAbiMaxBuffers)
         return kIOReturnBadArgument;
      struct drm_msm_gem_submit_bo bos[NativeAdrenoAbiMaxBuffers + ARRAY_SIZE(rw.hosts) + ARRAY_SIZE(rw.surfs)];
      struct drm_msm_gem_submit_cmd cmds[NativeAdrenoAbiMaxBuffers];
      unsigned nbos = 0;
      for (unsigned i = 0; i < s->buffer_count; i++) {
         const NativeAdrenoAbiSubmitBuffer *b = &s->buffers[i];
         uint64_t base = rw_bo_iova(b->handle);
         if (!base || b->iova < base)
            return kIOReturnBadArgument;
         unsigned at = 0;
         while (at < nbos && bos[at].handle != (uint32_t)b->handle)
            at++;
         if (at == nbos)
            bos[nbos++] = (struct drm_msm_gem_submit_bo){.flags = MSM_SUBMIT_BO_READ, .handle = (uint32_t)b->handle, .presumed = base};
         cmds[i] = (struct drm_msm_gem_submit_cmd){.type = MSM_SUBMIT_CMD_BUF, .submit_idx = at,
                                                   .submit_offset = (uint32_t)(b->iova - base), .size = (uint32_t)b->bytes};
      }
      /* the caller's own pages (RW_HOST) in every submission: the kernel pushes them out of the cache for the GPU to read */
      pthread_mutex_lock(&rw.lock);
      for (unsigned h = 0; h < rw.host_count; h++) {
         unsigned at = 0;
         while (at < nbos && bos[at].handle != rw.hosts[h])
            at++;
         if (at == nbos) {
            struct rw_bo *hb = rw_bo_slot(rw.hosts[h], false);
            bos[nbos++] = (struct drm_msm_gem_submit_bo){.flags = MSM_SUBMIT_BO_READ, .handle = rw.hosts[h],
                                                         .presumed = hb ? hb->iova : 0};
         }
      }
      for (unsigned h = 0; h < rw.surf_count; h++) {
         unsigned at = 0;
         while (at < nbos && bos[at].handle != rw.surfs[h])
            at++;
         if (at == nbos) {
            struct rw_bo *sb = rw_bo_slot(rw.surfs[h], false);
            /* (0x8000, RedwoodGPU's GPU_SUBMIT_BO_TRACK: named for the "drawn" mark only, no cache push -- the plugin pushes
             * the surfaces a command uses itself, and a push of every surface of the process at every submission would cost) */
            bos[nbos++] = (struct drm_msm_gem_submit_bo){.flags = MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE | 0x8000, .handle = rw.surfs[h],
                                                         .presumed = sb ? sb->iova : 0};
         }
      }
      pthread_mutex_unlock(&rw.lock);
      if (s->wait_fence) {
         NativeAdrenoAbiWaitFence w = {.fence = s->wait_fence, .timeout_ns = s->wait_timeout_ns};
         kern_return_t kr = rw_call(NativeAdrenoMethodWaitFence, &w, sizeof(w), NULL, 0);
         if (kr != KERN_SUCCESS)
            return kr;
      }
      struct drm_msm_gem_submit req = {.flags = MSM_PIPE_3D0, .nr_bos = nbos, .nr_cmds = s->buffer_count,
                                       .bos = (uint64_t)(uintptr_t)bos, .cmds = (uint64_t)(uintptr_t)cmds,
                                       .queueid = rw.queue};
      kern_return_t kr = rw_raw(RW_SEL(DRM_MSM_GEM_SUBMIT), &req, sizeof(req));
      if (kr == KERN_SUCCESS)
         r->fence = req.fence;
      return kr;
   }
   case NativeAdrenoMethodWaitFence: {
      const NativeAdrenoAbiWaitFence *w = (const NativeAdrenoAbiWaitFence *)in;
      /* The kernel's clock is mach_absolute_time's (CLOCK_UPTIME_RAW), not CLOCK_MONOTONIC: on dada the two are ~52 s apart, and a
       * CLOCK_MONOTONIC deadline was always in the kernel's past (g2vk2: "until 28.278769 s, asked for at 80.051689 s: over before it
       * began"), so every wait returned at once and Turnip spun. */
      uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
      uint64_t deadline = w->timeout_ns > UINT64_MAX - now ? UINT64_MAX / 2 : now + w->timeout_ns;
      struct drm_msm_wait_fence req = {.fence = (uint32_t)w->fence, .queueid = rw.queue};
      req.timeout.tv_sec = (int64_t)(deadline / 1000000000ull);
      req.timeout.tv_nsec = (int64_t)(deadline % 1000000000ull);
      return rw_raw(RW_SEL(DRM_MSM_WAIT_FENCE), &req, sizeof(req));
   }
   case NativeAdrenoMethodGetTimestamp: {
      NativeAdrenoAbiTimestamp *t = (NativeAdrenoAbiTimestamp *)out;
      t->ticks = rw_param(MSM_PARAM_TIMESTAMP, 0);
      return KERN_SUCCESS;
   }
   case NativeAdrenoMethodGetStatus: {
      NativeAdrenoAbiStatus *st = (NativeAdrenoAbiStatus *)out;
      /* RedwoodGPU's fault count is the GPU's, not this client's: another process's fault (a poster's shader) made every client's
       * Turnip call its device lost (g2vk7..9: backboardd's surface imports then all failed with DeviceLost and the screen flickered).
       * A fault is logged; this client's own hangs still end its waits. */
      static uint64_t seen;
      uint64_t faults = rw_param(MSM_PARAM_FAULTS, 0);
      if (faults != seen) {
         rw_log("the GPU's fault count is %llu (was %llu)", (unsigned long long)faults, (unsigned long long)seen);
         seen = faults;
      }
      st->faults = 0;
      st->suspends = rw_param(MSM_PARAM_SUSPENDS, 0);
      return KERN_SUCCESS;
   }
   case NativeAdrenoMethodSetMetadata:
   case NativeAdrenoMethodGetMetadata: {
      const NativeAdrenoAbiMetadata *m = (const NativeAdrenoAbiMetadata *)in;
      pthread_mutex_lock(&rw.lock);
      struct rw_bo *b = rw_bo_slot((uint32_t)m->handle, false);
      kern_return_t kr = b && b->handle ? KERN_SUCCESS : kIOReturnBadArgument;
      if (kr == KERN_SUCCESS && method == NativeAdrenoMethodSetMetadata) {
         b->metadata_bytes = MIN2(m->bytes, (uint32_t)NativeAdrenoAbiMetadataBytes);
         memcpy(b->metadata, m->text, b->metadata_bytes);
      } else if (kr == KERN_SUCCESS) {
         NativeAdrenoAbiMetadata *r = (NativeAdrenoAbiMetadata *)out;
         r->handle = m->handle;
         r->bytes = MIN2(b->metadata_bytes, m->bytes);
         memcpy(r->text, b->metadata, r->bytes);
      }
      pthread_mutex_unlock(&rw.lock);
      return kr;
   }
   case NativeAdrenoMethodCreateQueue: {
      NativeAdrenoAbiQueueReply *r = (NativeAdrenoAbiQueueReply *)out;
      r->queue = 1;
      return KERN_SUCCESS;
   }
   case NativeAdrenoMethodCloseQueue:
      return KERN_SUCCESS;
   default:
      return kIOReturnUnsupported;
   }
}

/* RedwoodGPU's connection, when the native service is not there. */
static bool
rw_open(void)
{
   io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(RW_SERVICE));
   if (!service)
      return false;
   kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &dw.conn);
   IOObjectRelease(service);
   if (kr != KERN_SUCCESS) {
      dw.conn = 0;
      return false;
   }
   rw.queue = 0;
   return true;
}
