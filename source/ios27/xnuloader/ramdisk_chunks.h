#ifndef RAMDISK_CHUNKS_H
#define RAMDISK_CHUNKS_H
#include <stdint.h>
struct ramdisk_chunks { uint64_t written, total; };
static int ramdisk_chunks_begin(struct ramdisk_chunks *s, uint64_t offset,
                               uint64_t total, uint64_t capacity)
{
    if (!total || total > capacity || offset >= total) return 0;
    if (offset) {
        if (s->written != offset || s->total != total) return 0;
    } else {
        s->written = 0;
        s->total = total;
    }
    return 1;
}
static int ramdisk_chunks_commit(struct ramdisk_chunks *s, uint64_t bytes)
{
    if (!bytes || s->written > s->total || bytes > s->total - s->written)
        return -1;
    s->written += bytes;
    return s->written == s->total;
}
#endif
