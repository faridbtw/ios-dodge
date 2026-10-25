/* Explicit RAM-probe recovery for Dodge. No storage or boot-slot writes.
 * PSCI SYSTEM_RESET uses the SMC conduit in the matched Dodge platform.
 * Disabled unless the one-core, CPU6 diagnostic explicitly requests a deadline.
 */
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>

extern void loader_log_line(const char *line);

static void
op13_probe_reset(thread_call_param_t p0, thread_call_param_t p1)
{
    (void)p0; (void)p1;
    loader_log_line("op13-probe: cold-reset deadline reached; PSCI SYSTEM_RESET\n");
    register uint64_t x0 __asm__("x0") = 0x84000009;
    register uint64_t x1 __asm__("x1") = 0;
    register uint64_t x2 __asm__("x2") = 0;
    register uint64_t x3 __asm__("x3") = 0;
    __asm__ volatile ("dsb sy\n\tsmc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
        : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
            "x14", "x15", "x16", "x17", "memory");
    printf("op13-probe: PSCI reset returned %#llx; existing resource-retention guards remain active\n", x0);
}

__startup_func static void
op13_probe_reset_init(void)
{
    uint32_t seconds = 0, board = 0, cpu = 0, cpus = 0;
    if (!PE_parse_boot_argn("sm8750_test_reset_seconds", &seconds, sizeof(seconds))) return;
    PE_parse_boot_argn("sm8750_gpu", &board, sizeof(board));
    PE_parse_boot_argn("bootcpu", &cpu, sizeof(cpu));
    PE_parse_boot_argn("cpus", &cpus, sizeof(cpus));
    if (board != 1 || cpu != 6 || cpus != 1 || seconds < 5 || seconds > 120) {
        printf("op13-probe: invalid cold-reset diagnostic configuration; reset not scheduled\n");
        return;
    }
    thread_call_t call = thread_call_allocate(op13_probe_reset, NULL);
    if (call == NULL) panic("op13-probe: cannot allocate cold-reset recovery timer");
    uint64_t deadline;
    clock_interval_to_deadline(seconds, NSEC_PER_SEC, &deadline);
    thread_call_enter_delayed(call, deadline);
    printf("op13-probe: explicit PSCI cold reset scheduled after %u seconds\n", seconds);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, op13_probe_reset_init);
