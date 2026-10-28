/* K90 SM8750 GPU on the OnePlus HackiOS loader. */
#include <kern/clock.h>
#include <machine/atomic.h>
#include <libkern/libkern.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include "adreno8_private.h"
#include "adreno8_memory_private.h"

extern void loader_log_line(const char *line);

/* Prime on the BSD initialization thread before starting GPU clients, as in
 * donor 2249d0b. Submission and framebuffer threads read only this result. */
static int oneplus_gpu_platform = -1;

void
oneplus_gpu_platform_init(void)
{
	DTEntry entry;
	const void *property;
	unsigned int bytes;
	bool matches = false;

	if (os_atomic_load(&oneplus_gpu_platform, acquire) >= 0) {
		return;
	}
	if (SecureDTLookupEntry(NULL, "/arm-io/gic", &entry) == kSuccess &&
	    SecureDTGetProperty(entry, "reg", &property, &bytes) == kSuccess && bytes == 4 * sizeof(uint64_t)) {
		const uint64_t *reg = property;
		matches = pe_arm_get_soc_base_phys() == 0x00400000 && reg[0] == 0x15c00000 &&
		    reg[1] == 0x10000 && reg[2] == 0x15c80000 && reg[3] == 0x200000;
	}
	os_atomic_store(&oneplus_gpu_platform, matches ? 1 : 0, release);
	loader_log_line(matches ? "oneplus-gpu: immutable SM8750 platform verified and cached\n" :
	    "oneplus-gpu: loader platform rejected; GPU service remains disabled\n");
}

bool
redwood_platform_is_annibale(void)
{
	return os_atomic_load(&oneplus_gpu_platform, acquire) == 1;
}
bool redwood_platform_blocks_legacy_drivers(void) { return true; }
bool redwood_adreno_return_safe(void)
{
 return !adreno8_reset_required && !adreno8_memory_retained();
}
