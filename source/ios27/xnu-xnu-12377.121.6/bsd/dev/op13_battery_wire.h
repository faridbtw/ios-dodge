/* OnePlus SM8750 PMIC GLINK get-only ABI. Numbers from the pinned vendor
 * charger/v2/charger_ic/oplus_hal_adsp.h; temperature/capacity units from
 * oplus_hal_adsp.c. No battery or charger setting commands are exposed. */
#ifndef OP13_BATTERY_WIRE_H
#define OP13_BATTERY_WIRE_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

enum {
 OP13_BC_OWNER = 32778, OP13_BC_REPLY = 1,
 OP13_BC_BATTERY_GET = 0x30, OP13_BC_USB_GET = 0x32,
 OP13_BC_STATUS = 0, OP13_BC_PRESENT = 2, OP13_BC_CAPACITY = 4,
 OP13_BC_VOLTAGE = 7, OP13_BC_CURRENT = 9, OP13_BC_TEMPERATURE = 12,
 OP13_BC_CHARGE = 14, OP13_BC_CYCLES = 15, OP13_BC_DESIGN = 16,
 OP13_BC_FULL = 17,
 OP13_BC_OEM_GET = 0x10000, OP13_BC_OEM_WORDS = 128,
 OP13_BC_USB_IN_STATUS = 50,
};
struct op13_bc_message { uint32_t owner, type, opcode, words[3]; };
_Static_assert(sizeof(struct op13_bc_message) == 24, "PMIC GLINK ABI size");
struct op13_bc_oem_request { uint32_t owner, type, opcode, size; };
struct op13_bc_oem_reply {
 uint32_t owner, type, opcode, words[OP13_BC_OEM_WORDS], size;
};
_Static_assert(sizeof(struct op13_bc_oem_reply) == 528, "OPlus OEM gauge ABI size");
static inline bool
op13_bc_oem_valid(const struct op13_bc_oem_reply *m, size_t length)
{
 /* Check the full fixed-size envelope before reading its trailing size. */
 return m && length == sizeof(*m) && m->owner == OP13_BC_OWNER &&
     m->type == OP13_BC_REPLY && m->opcode == OP13_BC_OEM_GET &&
     m->size >= 40 && m->size <= sizeof(m->words) && (m->size & 3) == 0;
}

static inline bool
op13_bc_get_allowed(uint32_t opcode, uint32_t property)
{
 if (opcode == OP13_BC_USB_GET)
  return property == 0 || property == 1 || property == 3 || property == 6 || property == OP13_BC_USB_IN_STATUS;
 if (opcode != OP13_BC_BATTERY_GET) return false;
 switch (property) {
 case OP13_BC_STATUS: case OP13_BC_PRESENT: case OP13_BC_CAPACITY:
 case OP13_BC_VOLTAGE: case OP13_BC_CURRENT: case OP13_BC_TEMPERATURE:
 case OP13_BC_CHARGE: case OP13_BC_CYCLES: case OP13_BC_DESIGN: case OP13_BC_FULL:
  return true;
 default: return false;
 }
}

static inline bool
op13_bc_make_get(struct op13_bc_message *out, uint32_t opcode, uint32_t property)
{
 if (!out || !op13_bc_get_allowed(opcode, property)) return false;
 *out = (struct op13_bc_message){ OP13_BC_OWNER, OP13_BC_REPLY, opcode, {0, property, 0} };
 return true;
}

static inline bool
op13_bc_reply_valid(const struct op13_bc_message *m, size_t length)
{
 return m && length == sizeof(*m) && m->owner == OP13_BC_OWNER &&
     m->type == OP13_BC_REPLY && op13_bc_get_allowed(m->opcode, m->words[0]);
}

/* Numeric units remain the firmware's: capacity 0..10000, microvolts,
 * signed microamps and hundredths of Celsius. Allow both one/two-cell voltage. */
static inline bool
op13_bc_sample_valid(uint32_t capacity, uint32_t present, uint32_t status,
    uint32_t voltage, int32_t current, int32_t temperature, uint32_t usb)
{
 return capacity <= 10000 && present == 1 && status <= 4 && usb <= 1 &&
     voltage >= 2000000 && voltage <= 12000000 &&
     current >= -30000000 && current <= 30000000 &&
     temperature >= -4000 && temperature <= 10000;
}
#endif
