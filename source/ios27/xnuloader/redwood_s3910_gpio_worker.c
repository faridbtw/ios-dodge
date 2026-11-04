/* SPDX-License-Identifier: BSD-2-Clause
 * Dedicated OnePlus 13 driver; Goodix/FocalTech retain touchscreen=1.
 * Select this driver using touchscreen=2. Only the worker may touch the bus.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/redwood_s3910.h>
#include <sys/systm.h>

extern void loader_log_putc(char);
static int touch_say(const char *format,...) __printflike(1,2);
static int touch_say(const char *format,...)
{
 char line[400];va_list ap;va_start(ap,format);
 vsnprintf(line,sizeof(line),format,ap);va_end(ap);
 for(const char *c=line;*c;c++)loader_log_putc(*c);
 return printf("%s",line);
}
#define printf touch_say

extern int sm8750_touch_bus_open(struct s3910_io *);
extern void sm8750_touch_bus_close(void);
extern kern_return_t thread_terminate(thread_t);
static struct s3910 controller;
static unsigned diagnostic_events;

static void
post(void *cookie, unsigned type, unsigned slot, uint32_t x, uint32_t y)
{
 (void)cookie;
 if (diagnostic_events++ < 32)
  printf("s3910: event type=%u slot=%u normalized=%u,%u\n", type, slot, x, y);
 redwood_touch_post(type, slot, x, y);
}

static void
s3910_worker(void *parameter, wait_result_t result)
{
 struct s3910_io io = { 0 };
 int error, poll_us = 4000, stage = 1;
 (void)parameter; (void)result;
 PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
 if (poll_us < 1000 || poll_us > 20000) poll_us = 4000;
 PE_parse_boot_argn("touch_steps", &stage, sizeof(stage));
 if (stage < 1 || stage > 4) {
  printf("s3910: touch_steps must be 1..4; no hardware access\n");
  goto done;
 }
 printf("s3910: requested stage %d; waiting for verified resource provider\n", stage);
 io.event = post;
 error = sm8750_touch_bus_open(&io);
 if (error) {
  printf("s3910: board bus unavailable (%d); no controller commands issued\n", error);
  goto done;
 }
 error = s3910_probe(&controller, &io);
 printf("s3910: startup header=%02x %02x %02x %02x version=%02x mode=%02x "
  "part=%.16s extension=%02x %02x %02x %02x\n",
  controller.startup_header[0], controller.startup_header[1],
  controller.startup_header[2], controller.startup_header[3],
  controller.identification[0], controller.identification[1],
  (const char *)&controller.identification[2],
  controller.startup_extension[0], controller.startup_extension[1],
  controller.startup_extension[2], controller.startup_extension[3]);
 if (error) {
  printf("s3910: startup identification refused (%d): need app firmware, v1, no CRC/RC\n", error);
  goto done;
 }
 if (stage == 1) { printf("s3910: stage1 identify passed, max_write=%u\n", controller.max_write); goto done; }
 error = s3910_read_app_info(&controller);
 if (error) {
  printf("s3910: app info refused error=%d cmd=%#x status=%#x length=%u\n", error, controller.last_command, controller.last_status, controller.length);
  goto done;
 }
 printf("s3910: stage2 APP bounds=%ux%u slots=%u config=%u payload=%u\n", controller.max_x, controller.max_y, controller.max_objects, controller.config_size, controller.max_payload);
 if (stage == 2) goto done;
 error = s3910_enable_lbp(&controller);
 if (error) {
  printf("s3910: LBP setup refused (%d), command=%#x response=%#x\n",
   error, controller.last_command, controller.last_status);
  goto done;
 }
 printf("s3910: LBP ready, bounds %ux%u, %u slots, IRQ poll %dus\n",
  controller.max_x, controller.max_y, controller.max_objects, poll_us);
 if (stage == 3) { printf("s3910: stage3 LBP/readback passed; polling not started\n"); goto done; }
 for (;;) {
  error = s3910_poll(&controller);
  if (error) {
   printf("s3910: stopped (%d), code=%#x frames=%u bad=%u resets=%u hbp=%u\n",
    error, controller.code, controller.frames, controller.bad_frames,
    controller.resets, controller.ignored_hbp);
   break;
  }
  delay_for_interval((uint32_t)poll_us, NSEC_PER_USEC);
 }
done:
 s3910_release_all(&controller);
 sm8750_touch_bus_close();
 thread_terminate(current_thread());
}

void
redwood_s3910_init(void)
{
 int selected = 0;
 thread_t thread;
 PE_parse_boot_argn("touchscreen", &selected, sizeof(selected));
 if (selected != 2) return;
 printf("s3910: OnePlus13 experimental LBP driver selected\n");
 if (kernel_thread_start(s3910_worker, NULL, &thread) == KERN_SUCCESS)
  thread_deallocate(thread);
 else printf("s3910: cannot create worker\n");
}
