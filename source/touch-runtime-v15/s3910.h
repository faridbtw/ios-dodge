/* SPDX-License-Identifier: BSD-2-Clause
 * Independently implemented from the public TouchCom interface definitions.
 * No Linux driver code is included. See REFERENCES.md for protocol sources.
 */
#ifndef SM8750_S3910_H
#define SM8750_S3910_H
#include <stddef.h>
#include <stdint.h>

#define S3910_MAX_OBJECTS 10
#define S3910_MAX_PAYLOAD 1024
#define S3910_REPORT_TOUCH 0x11
#define S3910_REPORT_IDENTIFY 0x10
#define S3910_REPORT_HBP 0x23
enum s3910_error {
 S3910_OK = 0, S3910_IO = -1, S3910_PROTOCOL = -2,
 S3910_UNSUPPORTED = -3, S3910_TIMEOUT = -4, S3910_RANGE = -5,
 S3910_NOT_READY = -6, S3910_RESET = -7
};
enum s3910_event { S3910_DOWN = 1, S3910_MOVE = 2, S3910_UP = 3 };

/* All methods run on ONE worker thread, never in interrupt context.
 * SPI read clocks 0xff with CS asserted for the entire transfer; write likewise.
 * Return zero only for an exact-length transfer. irq_asserted returns 0/1 or <0.
 * The board binding must supply time-bounded I/O and correctly powered hardware.
 */
struct s3910_io {
 void *cookie;
 int (*read)(void *, uint8_t *, size_t);
 int (*write)(void *, const uint8_t *, size_t);
 int (*irq_asserted)(void *);
 void (*sleep_ms)(void *, unsigned);
 void (*event)(void *, unsigned, unsigned, uint32_t, uint32_t);
};
struct s3910_slot { uint16_t x, y; uint8_t active; };
struct s3910 {
 struct s3910_io io;
 struct s3910_slot slots[S3910_MAX_OBJECTS];
 uint8_t payload[S3910_MAX_PAYLOAD];
 uint16_t length, max_write, max_x, max_y, max_objects, config_size, max_payload;
 uint8_t code, ready, identified, last_command, last_status;
 uint8_t startup_header[4], identification[32], startup_extension[4];
 uint16_t startup_id_length;
 uint32_t frames, bad_frames, resets, ignored_hbp;
};

/* Read bounded24..32-byte startup identify, detect TouchCom v1 by its wire
 * marker, reject CRC/extra-RC/v2 framing; identification length does not itself
 * select the wire protocol. No firmware downloads or flash writes.
 */
int s3910_probe(struct s3910 *, const struct s3910_io *);
int s3910_read_app_info(struct s3910 *); /* GET_APP_INFO only; no report/config changes */
int s3910_enable_lbp(struct s3910 *);
int s3910_poll(struct s3910 *);
void s3910_release_all(struct s3910 *);
/* Exposed for deterministic decoder tests; requires ready=1 after setup. */
int s3910_touch_payload(struct s3910 *, const uint8_t *, size_t);
#endif
