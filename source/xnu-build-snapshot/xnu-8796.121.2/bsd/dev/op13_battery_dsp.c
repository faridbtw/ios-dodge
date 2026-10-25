/* Adapted from hxfuxyy/ios-macan bf6106be492670acd05fac0ee7b7d037a48eafd9.
 * Original code retains its upstream terms (repository: XNU-derived APSL 2.0).
 * Dodge SM8750 configuration: captured Android FDT, diagnostics/20261025-battery.
 * Only read-only survey / firmware-validation phases are currently permitted.
 */
#include <sys/op13_battery_dsp.h>
#include <os/atomic_private.h>
static LCK_GRP_DECLARE(dsp_shared_group, "macan-dsp-shared");
LCK_MTX_DECLARE(op13_batt_aop_mailbox_lock, &dsp_shared_group);
LCK_MTX_DECLARE(op13_batt_smem_alloc_lock, &dsp_shared_group);
LCK_MTX_DECLARE(op13_batt_rpmh_lock, &dsp_shared_group);
static uint32_t ipcc_lock;
static uint32_t pending[256];
static uint32_t
ipcc_read(vm_offset_t base)
{
 return *(volatile uint32_t *)(base + 0x10);
}
static void
ipcc_clear(vm_offset_t base, uint32_t who)
{
 *(volatile uint32_t *)(base + 0x1c) = who;
 __asm__ volatile ("dsb sy" : : : "memory");
}
uint32_t
op13_batt_ipcc_take(vm_offset_t base, uint32_t client)
{
 uint32_t result = 0xffffffffu;
 if (!base || client >= 256) { return result; }
 if (!os_atomic_cmpxchg(&ipcc_lock, 0, 1, acquire)) { return result; }
 for (unsigned n = 0; n < 64; n++) {
  uint32_t who = ipcc_read(base);
  if (who == 0xffffffffu) { break; }
  uint32_t c = who >> 16, signal = who & 0xffff;
  if (c >= 256 || signal >= 32) { break; }
  pending[c] |= 1u << signal;
  ipcc_clear(base, who);
 }
 if (pending[client]) {
  unsigned signal = (unsigned)__builtin_ctz(pending[client]);
  pending[client] &= ~(1u << signal);
  result = client << 16 | signal;
 }
 os_atomic_store(&ipcc_lock, 0, release);
 return result;
}
