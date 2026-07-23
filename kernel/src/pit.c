#include <stddef.h>
#include "pit.h"
#include "io.h"
#include "isr.h"
#include "pic.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_BASE_FREQUENCY 1193182u /* fixed input clock all PIT dividers are relative to */

static volatile uint64_t ticks = 0;
static irq_handler_t tick_callback = NULL;

static void pit_handler(struct registers *regs) {
    ticks++;
    if (tick_callback != NULL) {
        tick_callback(regs);
    }
}

void pit_init(uint32_t frequency_hz) {
    uint16_t divisor = (uint16_t)(PIT_BASE_FREQUENCY / frequency_hz);
    outb(PIT_COMMAND, 0x36); /* channel 0, lobyte/hibyte access, mode 3 (square wave) */
    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF);

    irq_register_handler(0, pit_handler);
    pic_clear_mask(0);
}

uint64_t pit_get_ticks(void) {
    return ticks;
}

void pit_set_tick_callback(irq_handler_t callback) {
    tick_callback = callback;
}
