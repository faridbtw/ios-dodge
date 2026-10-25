/* SPDX-License-Identifier: BSD-2-Clause */
#ifdef KERNEL
#include <sys/redwood_s3910.h>
#else
#include "s3910.h"
#endif

/* Same byte-aligned entities as the vendor's custom_touch_format, without
 * optional gesture ID. Firmware accepts this config before any touch is parsed.
 */
static const uint8_t touch_config[] = {
 0x18, 8, 0x01, 0x06, 8, 0x07, 8, 0x08, 16, 0x09, 16, 0x03, 0x00
};
static unsigned le16(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8); }
static void zero(void *p, size_t n) { uint8_t *b = p; while (n--) *b++ = 0; }
static int equal(const uint8_t *a, const uint8_t *b, size_t n)
{ while (n--) if (*a++ != *b++) return 0; return 1; }

void s3910_release_all(struct s3910 *d)
{
 unsigned i;
 for (i = 0; i < S3910_MAX_OBJECTS; i++) {
  if (d->slots[i].active && d->io.event)
   d->io.event(d->io.cookie, S3910_UP, i,
    (uint32_t)d->slots[i].x * 65535u / d->max_x,
    (uint32_t)d->slots[i].y * 65535u / d->max_y);
  d->slots[i].active = 0;
 }
}

/* A v1 packet is header A5/code/length-le16, then a separate continued read
 * A5/03/payload/5A. A zero-payload message ends at the initial header.
 */
static int receive(struct s3910 *d)
{
 uint8_t h[4], b[S3910_MAX_PAYLOAD + 3];
 unsigned i, n;
 if (d->io.read(d->io.cookie, h, sizeof(h))) return S3910_IO;
 if (h[0] != 0xa5) return S3910_PROTOCOL;
 d->code = h[1];
 d->length = (uint16_t)le16(h + 2);
 n = d->length;
 if (n > S3910_MAX_PAYLOAD) return S3910_RANGE;
 if (n) {
  if (d->io.read(d->io.cookie, b, n + 3)) return S3910_IO;
  if (b[0] != 0xa5 || b[1] != 3 || b[n + 2] != 0x5a)
   return S3910_PROTOCOL;
  for (i = 0; i < n; i++) d->payload[i] = b[i + 2];
 }
 if (d->code == 3 || d->code == 0xff) return S3910_PROTOCOL;
 return S3910_OK;
}

static int wait_irq(struct s3910 *d)
{
 unsigned i;
 for (i = 0; i < 1000; i++) {
  int r = d->io.irq_asserted(d->io.cookie);
  if (r < 0) return S3910_IO;
  if (r) return 0;
  d->io.sleep_ms(d->io.cookie, 1);
 }
 return S3910_TIMEOUT;
}

int s3910_probe(struct s3910 *d, const struct s3910_io *io)
{
 uint8_t h[4], b[32 + 7];
 unsigned i, n;
 int r;
 if (!d || !io || !io->read || !io->write || !io->irq_asserted ||
     !io->sleep_ms || !io->event) return S3910_RANGE;
 zero(d, sizeof(*d));
 d->io = *io;
 r = wait_irq(d);
 if (r) return r;
 if (io->read(io->cookie, h, 4)) return S3910_IO;
 for (i = 0; i < sizeof(h); i++) d->startup_header[i] = h[i];
 /* The vendor v1 detector uses A5 framing and copies MIN(id struct,payload).
  * OP13 hardware returns28 bytes, extending the common24-byte prefix. The
  * length is not a v1/v2 protocol discriminator; bound it before continuing. */
 n=le16(h+2);d->startup_id_length=(uint16_t)n;
 if (h[0] != 0xa5 || h[1] != 0x10 || n < 24 || n > 32)
  return S3910_UNSUPPORTED;
 /* Vendor probes optional CRC (2), extra RC (1), final EOM (1).
  * This first implementation supports only the no-extension wire variant.
  */
 if (io->read(io->cookie, b, n+7)) return S3910_IO;
 for (i = 0; i < n; i++) d->identification[i] = b[i + 2];
 for (i = 0; i < sizeof(d->startup_extension); i++) d->startup_extension[i] = b[i + n + 3];
 if (b[0] != 0xa5 || b[1] != 3 || b[n+2] != 0x5a)
  return S3910_PROTOCOL;
 for (i = n+3; i < n+7; i++)
  if (b[i] != 0x5a) return S3910_UNSUPPORTED;
 if (b[3] != 1) return S3910_NOT_READY; /* application firmware only */
 d->max_write = (uint16_t)le16(b + 24);
 if (d->max_write && d->max_write < 6) return S3910_UNSUPPORTED;
 d->identified = 1;
 return 0;
}

static int command(struct s3910 *d, uint8_t cmd, const uint8_t *p, unsigned n)
{
 uint8_t b[S3910_MAX_PAYLOAD + 3];
 unsigned i;
 int r;
 if (!d->identified) return S3910_NOT_READY;
 if (n > S3910_MAX_PAYLOAD || (d->max_write && n + 3 > d->max_write))
  return S3910_RANGE; /* chunked writes deliberately not supported */
 b[0] = cmd; b[1] = (uint8_t)n; b[2] = (uint8_t)(n >> 8);
 for (i = 0; i < n; i++) b[i + 3] = p[i];
 d->last_command = cmd;
 d->last_status = 0xff;
 if (d->io.write(d->io.cookie, b, n + 3)) return S3910_IO;
 /* Limit reports interleaved with the command response and wall-clock wait. */
 for (i = 0; i < 1000; i++) {
  r = d->io.irq_asserted(d->io.cookie);
  if (r < 0) return S3910_IO;
  if (!r) { d->io.sleep_ms(d->io.cookie, 1); continue; }
  r = receive(d);
  if (r) return r;
  if (d->code == 0x10) {
   d->identified = d->ready = 0; d->resets++;
   s3910_release_all(d);
   return S3910_RESET;
  }
  if (d->code == 1) { d->last_status = 1; return 0; }
  if (d->code && d->code < 0x10) {
   d->last_status = d->code;
   return S3910_UNSUPPORTED;
  }
  if (d->code == S3910_REPORT_HBP) d->ignored_hbp++;
  /* Setup drops all report data until layout readback succeeds. */
  d->io.sleep_ms(d->io.cookie, 1);
 }
 return S3910_TIMEOUT;
}

int s3910_read_app_info(struct s3910 *d)
{
 int r;
 r = command(d, 0x20, 0, 0); /* GET_APPLICATION_INFO */
 if (r) return r;
 if (d->length < 38 || le16(d->payload + 2) != 0) return S3910_NOT_READY;
 if (le16(d->payload) != 1) return S3910_UNSUPPORTED;
 d->config_size = (uint16_t)le16(d->payload + 12);
 d->max_payload = (uint16_t)le16(d->payload + 14);
 d->max_x = (uint16_t)le16(d->payload + 32);
 d->max_y = (uint16_t)le16(d->payload + 34);
 d->max_objects = (uint16_t)le16(d->payload + 36);
 if (!d->max_x || !d->max_y || !d->max_objects ||
     d->max_objects > S3910_MAX_OBJECTS ||
     d->max_payload < 1 + d->max_objects * 6 ||
     d->config_size < sizeof(touch_config) || d->config_size > S3910_MAX_PAYLOAD)
  return S3910_UNSUPPORTED;
 return 0;
}

int s3910_enable_lbp(struct s3910 *d)
{
 uint8_t config[S3910_MAX_PAYLOAD], report, dc[3] = { 0x11, 2, 0 };
 unsigned i;
 int r;
 d->ready = 0;
 s3910_release_all(d);
 r = s3910_read_app_info(d);
 if (r) return r;
 /* Exact vendor HBP->LBP sequence, then read back dynamic config. */
 report = S3910_REPORT_HBP;
 if ((r = command(d, 0x06, &report, 1))) return r;
 if ((r = command(d, 0x24, dc, sizeof(dc)))) return r;
 if ((r = command(d, 0x23, dc, 1))) return r;
 if (d->length != 2 || le16(d->payload) != 2) return S3910_PROTOCOL;
 report = S3910_REPORT_TOUCH;
 if ((r = command(d, 0x06, &report, 1))) return r;
 zero(config, d->config_size);
 for (i = 0; i < sizeof(touch_config); i++) config[i] = touch_config[i];
 if ((r = command(d, 0x26, config, d->config_size))) return r;
 if ((r = command(d, 0x25, 0, 0))) return r;
 if (d->length < sizeof(touch_config) ||
     !equal(d->payload, touch_config, sizeof(touch_config))) return S3910_PROTOCOL;
 if ((r = command(d, 0x05, &report, 1))) return r;
 d->ready = 1;
 return 0;
}

int s3910_touch_payload(struct s3910 *d, const uint8_t *p, size_t n)
{
 struct s3910_slot next[S3910_MAX_OBJECTS];
 unsigned i, count, seen = 0;
 if (!d->ready) return S3910_NOT_READY;
 if (!p || n < 1 || !d->max_x || !d->max_y) goto bad;
 count = p[0];
 if (count > d->max_objects || count > S3910_MAX_OBJECTS || n > d->max_payload || n != 1 + count * 6)
  goto bad;
 zero(next, sizeof(next));
 /* Validate the WHOLE frame before changing slots or emitting any event. */
 for (i = 0; i < count; i++) {
  const uint8_t *q = p + 1 + i * 6;
  unsigned id = q[0], type = q[1], x = le16(q + 2), y = le16(q + 4);
  if (id >= d->max_objects || id >= S3910_MAX_OBJECTS || (seen & (1u << id)) ||
      x > d->max_x || y > d->max_y) goto bad;
  seen |= 1u << id;
  /* Palm/hover/stylus are not converted to finger touches. */
  if (type == 1 || type == 2) {
   next[id].active = 1; next[id].x = (uint16_t)x; next[id].y = (uint16_t)y;
  }
 }
 for (i = 0; i < S3910_MAX_OBJECTS; i++) {
  struct s3910_slot *old = &d->slots[i], *now = &next[i];
  unsigned event = 0, x = now->x, y = now->y;
  if (now->active) {
   if (!old->active) event = S3910_DOWN;
   else if (old->x != now->x || old->y != now->y) event = S3910_MOVE;
  } else if (old->active) { event = S3910_UP; x = old->x; y = old->y; }
  if (event) d->io.event(d->io.cookie, event, i,
   (uint32_t)x * 65535u / d->max_x, (uint32_t)y * 65535u / d->max_y);
  *old = *now;
 }
 d->frames++;
 return 0;
bad:
 d->bad_frames++;
 return S3910_PROTOCOL;
}

int s3910_poll(struct s3910 *d)
{
 int r;
 if (!d->ready) return S3910_NOT_READY;
 r = d->io.irq_asserted(d->io.cookie);
 if (r < 0) return S3910_IO;
 if (!r) return 0;
 r = receive(d);
 if (r) goto stop;
 if (d->code == S3910_REPORT_TOUCH) {
  r = s3910_touch_payload(d, d->payload, d->length);
  if (r) goto stop;
 } else if (d->code == S3910_REPORT_IDENTIFY) {
  d->identified = 0; d->resets++; r = S3910_RESET; goto stop;
 } else if (d->code == S3910_REPORT_HBP) {
  d->ignored_hbp++; r = S3910_UNSUPPORTED; goto stop;
 }
 return 0;
stop:
 d->ready = 0;
 s3910_release_all(d);
 return r;
}
