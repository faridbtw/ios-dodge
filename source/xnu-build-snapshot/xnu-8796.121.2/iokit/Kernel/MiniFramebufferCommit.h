/* SPDX-License-Identifier: BSD-2-Clause
 * Bounded read-only panel-TE wait and complete-frame copy for RAM scanout.
 */
#ifndef MINI_FRAMEBUFFER_COMMIT_H
#define MINI_FRAMEBUFFER_COMMIT_H
#include <stdint.h>
#include <stddef.h>

struct mini_fb_te {
    uint64_t last, period;
    unsigned failures, enabled;
};
struct mini_fb_te_io {
    void *cookie;
    uint64_t (*now)(void *);
    unsigned (*level)(void *);
    void (*sleep_until)(void *, uint64_t);
};

static inline int mini_fb_wait_te(struct mini_fb_te *s, const struct mini_fb_te_io *io)
{
    if (!s || !s->enabled || !io || !io->now || !io->level || !io->sleep_until) return 0;
    uint64_t now = io->now(io->cookie);
    const uint64_t budget = s->period ? 25000000ULL : 50000000ULL;
    if (now > UINT64_MAX - budget) return 0;
    const uint64_t end = now + budget;
    if (s->last && s->period >= 4000000ULL && s->period <= 25000000ULL) {
        uint64_t next = s->last;
        if (now >= next) next += ((now-next)/s->period + 1)*s->period;
        if (next > now + 750000ULL && next < end)
            io->sleep_until(io->cookie, next - 750000ULL);
    }
    unsigned previous = io->level(io->cookie) != 0;
    while ((now = io->now(io->cookie)) < end) {
        unsigned current = io->level(io->cookie) != 0;
        if (current && !previous) {
            if (!s->period && s->last) {
                const uint64_t period = now - s->last;
                if (period >= 4000000ULL && period <= 25000000ULL) s->period = period;
            }
            s->last = now;
            if (s->period) { s->failures = 0; return 1; }
        }
        previous = current;
    }
    s->last = s->period = 0;
    if (++s->failures >= 3) s->enabled = 0;
    return 0;
}

static inline int mini_fb_commit_rows(void *front, const void *frame,
    uint32_t width, uint32_t height, uint32_t stride,
    void (*copy)(void *, const void *, size_t))
{
    if (!front || !frame || !copy || !width || !height || width > 16384 || height > 16384 ||
        stride < (uint64_t)width * 4 || height > SIZE_MAX / stride) return 0;
    size_t bytes = (size_t)height * stride;
    uintptr_t a = (uintptr_t)front, b = (uintptr_t)frame;
    if (a > UINTPTR_MAX-bytes || b > UINTPTR_MAX-bytes || (a < b+bytes && b < a+bytes)) return 0;
    for (uint32_t y=0; y<height; y++)
        copy((uint8_t *)front+(size_t)y*stride, (const uint8_t *)frame+(size_t)y*stride, (size_t)width*4);
    return 1;
}
#endif
